#pragma once

#include <sys/socket.h>
#include <linux/in.h>
#include <sys/endian.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <ifaddrs.h>
#include <linux/if.h>
#include <vector>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <signal.h>
#include <string_view>

#include "facial-tracking.hpp"

#define PORT 9030
#define MULTICAST_ADDRESS "239.255.255.250"

#define DISCOVER_PING "DISCOVER_DAEMON"

// hehe
#define PING "MARCO"
#define REPLY "POLO"
#define DAEMON_STOP "STOP"

class FacialTrackingSocket
{
public:
    FacialTrackingSocket();
    void Listen();

private:
    /**
     * Manually polls the shared memory data buffer, which is pretty much how it is done through the Unity API.
     * Adding a service listener doesn't work, so this is a stand-in replacement.
     */
    void Poll(std::chrono::nanoseconds pollInterval);
    /**
     * @returns 1 if a sample was sent, 0 if there was nothing to send, -1 on a socket error.
     */
    int Send();
    void Ping();
    void WaitForStop();

    void RegisterSigKillHandler();
    static void SigKillHandler(int signalNumber);

    bool Discover(sockaddr_in *client);
    void SetupClientSocket();

    int facialDataSocket = -1;
    std::atomic<bool> connected{false};
    std::atomic<bool> active{false};
    std::atomic<bool> kill{false};

    std::atomic<bool> stopThreadRunning{false};
    std::condition_variable cv;
    std::mutex cvMutex;

    FacialTracking *facialTracking;
};