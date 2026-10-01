#include <thread>

#include "log.hpp"
#include "facial-tracking-socket.hpp"

static constexpr std::chrono::seconds PING_INTERVAL(25);
static constexpr std::chrono::seconds PING_INTERVAL_INACTIVE(1);
static constexpr std::chrono::seconds PING_TIMEOUT(5);
static constexpr std::chrono::milliseconds MIN_SEND_INTERVAL(30);

// No new sample for this long means that the algorithm stopped: the headset went to sleep
// (pxreyetrackingservice stops the algorithm then) or the service restarted, which unmaps the
// old shared memory. The algorithm is started again until it succeeds.
static constexpr std::chrono::seconds STALE_DATA_TIMEOUT(2);

// Discovery waits this long before it joins the multicast group again, so that it recovers
// from Wi-Fi reconnects (the membership is bound to the interface that was up when joining).
static constexpr int DISCOVER_TIMEOUT_SECONDS = 15;

FacialTrackingSocket::FacialTrackingSocket()
{
    this->facialTracking = new FacialTracking();
}

void FacialTrackingSocket::Listen()
{
    this->RegisterSigKillHandler();

    while (!this->kill.load())
    {
        // Discover the client from the multicast broadcast.
        sockaddr_in client{};
        if (!this->Discover(&client))
            continue;

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, clientIp, sizeof(clientIp));
        LOGI("Client discovered: %s", clientIp);

        this->facialDataSocket = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (this->facialDataSocket < 0)
        {
            LOGE("socket failed: %s", strerror(errno));
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // Control() polls the socket with this timeout.
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500 * 1000;
        setsockopt(this->facialDataSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

        connect(this->facialDataSocket, (struct sockaddr *)&client, sizeof(client));
        this->connected.store(true);
        this->active.store(false);

        std::thread controlThread(&FacialTrackingSocket::Control, this);

        // Blocking: starts the algorithm (again after sleep or a service restart) and sends data
        // until the ping thread detects no reply anymore or the client sends STOP.
        this->Poll(std::chrono::milliseconds(10));

        this->facialTracking->Stop();
        this->active.store(false);

        if (controlThread.joinable())
            controlThread.join();

        close(this->facialDataSocket);
        this->facialDataSocket = -1;

        LOGI("Client disconnected: %s", clientIp);
    }
}

void FacialTrackingSocket::Poll(std::chrono::nanoseconds pollInterval)
{
    auto lastData = std::chrono::steady_clock::now();

    while (this->connected.load())
    {
        if (!this->active.load())
        {
            // The headset might be sleeping, therefore we cannot start the eye tracking algorithm. We'll have to poll and wait...
            if (!this->facialTracking->Start())
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            this->active.store(true);
            lastData = std::chrono::steady_clock::now();
        }

        int sent = this->Send();
        if (sent < 0)
            return;

        auto now = std::chrono::steady_clock::now();
        if (sent > 0)
        {
            lastData = now;
        }
        else if (now - lastData > STALE_DATA_TIMEOUT)
        {
            LOGI("No tracking data for %llds, restarting the algorithm",
                 static_cast<long long>(STALE_DATA_TIMEOUT.count()));
            this->facialTracking->Stop();
            this->active.store(false);
            continue;
        }

        std::this_thread::sleep_for(pollInterval);
    }
}

void FacialTrackingSocket::Control()
{
    // The only reader of the client socket: answers to MARCO and STOP are both handled here, so a
    // POLO can no longer be consumed by a separate STOP reader (which made the ping fail and
    // dropped a live client). MARCO is sent every 25 s, every second while the algorithm is not
    // running (to keep the module waiting while the headset sleeps).
    auto lastReply = std::chrono::steady_clock::now();
    auto lastPing = std::chrono::steady_clock::time_point::min();

    while (this->connected.load())
    {
        auto now = std::chrono::steady_clock::now();
        auto interval = this->active.load() ? PING_INTERVAL : PING_INTERVAL_INACTIVE;

        if (now - lastPing >= interval)
        {
            send(this->facialDataSocket, PING, sizeof(PING), 0);
            lastPing = now;
        }

        if (now - lastReply > PING_INTERVAL + PING_TIMEOUT)
        {
            LOGI("Client did not answer the ping");
            this->connected.store(false);
            return;
        }

        char buffer[128];
        ssize_t bytesRead = recv(this->facialDataSocket, buffer, sizeof(buffer), 0);
        if (bytesRead <= 0)
            continue;

        std::string_view message(buffer, bytesRead);
        while (!message.empty() && message.back() == '\0')
            message.remove_suffix(1);

        if (message == REPLY)
        {
            lastReply = std::chrono::steady_clock::now();
        }
        else if (message == DAEMON_STOP)
        {
            LOGI("Client sent STOP");
            this->connected.store(false);
            return;
        }
    }
}

int FacialTrackingSocket::Send()
{
    PxrFTInfo *faceTrackingData;
    pxr_eyepose_data_v2_0 *eyeTrackingData;

    if (!this->facialTracking->GetFacialData(&faceTrackingData, &eyeTrackingData))
        return 0;

    // Face and eye samples arrive separately at about 25 Hz each; send at most one packet per
    // MIN_SEND_INTERVAL (the latest of both), as the upstream rate, not one per stream.
    auto now = std::chrono::steady_clock::now();
    if (now - this->lastSend < MIN_SEND_INTERVAL)
    {
        this->facialTracking->KeepPending();
        return 2;
    }
    this->lastSend = now;

    struct iovec iov[2];
    iov[0].iov_base = faceTrackingData;
    iov[0].iov_len = sizeof(PxrFTInfo);
    iov[1].iov_base = eyeTrackingData;
    iov[1].iov_len = sizeof(pxr_eyepose_data_v2_0);

    struct msghdr msg = {};
    msg.msg_name = 0;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    ssize_t bytesSent = sendmsg(this->facialDataSocket, &msg, 0);

    if (bytesSent < 0)
    {
        // The route to the client is gone while Wi-Fi reconnects; the ping thread decides
        // whether the client is lost.
        if (errno == ENETUNREACH || errno == EHOSTUNREACH || errno == ENETDOWN || errno == ECONNREFUSED)
            return 0;

        LOGE("sendmsg failed: %s", strerror(errno));
        return -1;
    }

    return 1;
}

bool FacialTrackingSocket::Discover(sockaddr_in *client)
{
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
    {
        LOGE("socket failed: %s", strerror(errno));
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return false;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

    struct timeval tv;
    tv.tv_sec = DISCOVER_TIMEOUT_SECONDS;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(PORT);

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0)
    {
        LOGE("bind failed: %s", strerror(errno));
        close(sock);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return false;
    }

    // To find what PC we can send the tracking data to, we join this multicast group, wait for a ping, and then start sending data to that IP.
    ip_mreq group{};
    group.imr_multiaddr.s_addr = inet_addr(MULTICAST_ADDRESS);
    group.imr_interface.s_addr = INADDR_ANY;

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&group, sizeof(group)) != 0)
    {
        // No network yet (Wi-Fi down or reconnecting).
        close(sock);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return false;
    }

    bool discovered = false;
    while (!this->kill.load())
    {
        char buffer[sizeof(DISCOVER_PING)];
        sockaddr_in sender_addr{};
        socklen_t sender_len = sizeof(sender_addr);

        ssize_t bytesReceived = recvfrom(
            sock,
            buffer,
            sizeof(buffer) - 1,
            0,
            (struct sockaddr *)&sender_addr,
            &sender_len);

        // Timeout: join the group again in case the network changed.
        if (bytesReceived < 0)
            break;

        if (std::string_view(buffer, bytesReceived) == std::string_view(DISCOVER_PING, sizeof(DISCOVER_PING) - 1))
        {
            *client = sender_addr;
            discovered = true;
            break;
        }
    }

    setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, (char *)&group, sizeof(group));
    close(sock);

    return discovered;
}

FacialTrackingSocket *instance = nullptr;

void FacialTrackingSocket::RegisterSigKillHandler()
{
    instance = this;

    struct sigaction sa{};
    sa.sa_handler = &FacialTrackingSocket::SigKillHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

void FacialTrackingSocket::SigKillHandler(int signalNumber)
{
    if ((signalNumber == SIGTERM || signalNumber == SIGINT) && instance != nullptr)
    {
        instance->connected.store(false);
        instance->kill.store(true);
    }
}
