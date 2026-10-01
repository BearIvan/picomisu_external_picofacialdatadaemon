#include <thread>

#include "log.hpp"
#include "facial-tracking-socket.hpp"

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

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(this->facialDataSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

        connect(this->facialDataSocket, (struct sockaddr *)&client, sizeof(client));
        this->connected.store(true);
        this->active.store(false);

        std::thread pingThread(&FacialTrackingSocket::Ping, this);

        // Blocking: starts the algorithm (again after sleep or a service restart) and sends data
        // until the ping thread detects no reply anymore or the client sends STOP.
        this->Poll(std::chrono::milliseconds(10));

        this->facialTracking->Stop();
        this->active.store(false);

        if (pingThread.joinable())
            pingThread.join();

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

void FacialTrackingSocket::Ping()
{
    while (this->connected.load())
    {
        bool pingReceived = false;
        for (int i = 0; i < 5; i++)
        {
            send(this->facialDataSocket, PING, sizeof(PING), 0);

            char buffer[128];
            ssize_t bytesRead = recv(this->facialDataSocket, buffer, sizeof(buffer), 0);

            if (bytesRead > 0)
            {
                if (std::string_view(buffer, bytesRead) == REPLY)
                {
                    pingReceived = true;
                }
                else if (std::string_view(buffer, bytesRead) == DAEMON_STOP)
                {
                    this->connected.store(false);

                    return;
                }

                break;
            }
        }

        if (!pingReceived)
        {
            this->connected.store(false);
            return;
        }

        this->stopThreadRunning.store(true);

        // While we wait, we spin up a STOP thread if the module sends STOP we can then immediately stop.
        std::thread stopThread(&FacialTrackingSocket::WaitForStop, this);

        // Ping every second when we are waiting for the headset to wake up, to keep the module active.
        std::this_thread::sleep_for(
            this->active.load() ? std::chrono::seconds(25) : std::chrono::seconds(1));

        // Once we are done, stop the thread that waits for a STOP signal.
        this->stopThreadRunning.store(false);
        this->cv.notify_all();

        if (stopThread.joinable())
            stopThread.join();
    }
}

void FacialTrackingSocket::WaitForStop()
{
    while (this->stopThreadRunning.load())
    {
        char buffer[4];
        ssize_t bytesRead = recv(this->facialDataSocket, buffer, sizeof(buffer), MSG_DONTWAIT);

        if (bytesRead > 0 && std::string_view(buffer, bytesRead) == DAEMON_STOP)
        {
            this->connected = false;

            return;
        }

        std::unique_lock<std::mutex> lock(this->cvMutex);

        bool stopped = this->cv.wait_for(lock, std::chrono::milliseconds(500), [this]
                                         { return !this->stopThreadRunning.load(); });

        if (stopped)
            break;
    }
}

int FacialTrackingSocket::Send()
{
    PxrFTInfo *faceTrackingData;
    pxr_eyepose_data_v2_0 *eyeTrackingData;

    if (!this->facialTracking->GetFacialData(&faceTrackingData, &eyeTrackingData))
        return 0;

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
