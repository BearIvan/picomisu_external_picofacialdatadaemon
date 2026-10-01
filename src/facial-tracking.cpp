#include "log.hpp"

#include <binder/Binder.h>
#include <binder/IBinder.h>
#include <utils/RefBase.h>
#include <binder/Parcel.h>
#include <thread>

#include "facial-tracking.hpp"

// TODO seperate into its OWN file!
enum EyeTrackingServiceListenerTransactions
{
    ON_FRAME_AVAILABLE = IBinder::FIRST_CALL_TRANSACTION,
    ON_ALGORITHM_RESULTS_AVAILABLE,
    ON_DEVICE_ERROR,
    ON_IPD_AVAILABLE,
    ON_GLASS_WEARABLE_AVAILABLE,
    ON_IPD_FULL_DATA_AVAILABLE
};

class EyeTrackingServiceListener : public BBinder
{
public:
    EyeTrackingServiceListener(FacialTracking *facialTracking)
    {
        this->facialTracking = facialTracking;
    }
    status_t onTransact(unsigned int code, const Parcel &data, Parcel *reply, unsigned int flags = 0) override
    {
        data.enforceInterface(String16(LISTENER_DESCRIPTOR));

        switch (code)
        {
        case ON_ALGORITHM_RESULTS_AVAILABLE:
            data.readInt32();

            // this->facialTracking->OnAlgorithmResultAvailable();

            reply->writeInt32(OK);

            return NO_ERROR;
        }

        return BBinder::onTransact(code, data, reply, flags);
    }

private:
    FacialTracking *facialTracking;
};

FacialTracking::FacialTracking()
{
    this->eyeTrackingServiceListener = new EyeTrackingServiceListener(this);
}

void FacialTracking::CloseBuffers()
{
    delete this->faceTrackingDataBuffer;
    this->faceTrackingDataBuffer = nullptr;

    delete this->eyeTrackingDataBuffer;
    this->eyeTrackingDataBuffer = nullptr;

    // These point into the unmapped shared memory.
    this->lastFaceTrackingData = nullptr;
    this->lastEyeTrackingData = nullptr;
}

bool FacialTracking::Start()
{
    status_t algorithmStatus = PxrEyeTrackingService::StartAlgorithm(5, EYE_TRACKING_ON | FACE_TRACKING_ON, 1000);

    if (algorithmStatus != OK)
        return false;

    // If someone can figure out why the service listener isn't working, please send a PR... for now I have to resort to polling the shared memory...
    // status_t status = PxrEyeTrackingService::AddServiceListener(this->eyeTrackingServiceListener);

    this->CloseBuffers();

    //  Face tracking data buffer.
    void *faceTrackingSharedMemory = nullptr;
    int faceTrackingDataBufferFd = -1;
    size_t faceTrackingDataBufferSize = 0;
    status_t sharedMemoryStatus = PxrEyeTrackingService::GetTrackingDataSharedMemory(SHARED_MEMORY_FACE_TRACKING, &faceTrackingDataBufferFd, &faceTrackingSharedMemory, &faceTrackingDataBufferSize);

    if (sharedMemoryStatus != OK)
        return false;

    this->faceTrackingDataBuffer = new DataBuffer(faceTrackingSharedMemory, faceTrackingDataBufferFd, faceTrackingDataBufferSize);

    void *eyeTrackingSharedMemory = nullptr;
    int eyeTrackingDataBufferFd = -1;
    size_t eyeTrackingDataBufferSize = 0;
    sharedMemoryStatus = PxrEyeTrackingService::GetTrackingDataSharedMemory(SHARED_MEMORY_EYE_TRACKING, &eyeTrackingDataBufferFd, &eyeTrackingSharedMemory, &eyeTrackingDataBufferSize);

    if (sharedMemoryStatus != OK)
    {
        this->CloseBuffers();
        return false;
    }

    this->eyeTrackingDataBuffer = new DataBuffer(eyeTrackingSharedMemory, eyeTrackingDataBufferFd, eyeTrackingDataBufferSize);

    LOGI("Tracking algorithm started");

    return true;
}

bool FacialTracking::Stop()
{
    this->CloseBuffers();

    return PxrEyeTrackingService::StopAlgorithm(5, EYE_TRACKING_ON | FACE_TRACKING_ON) == OK;
}

bool FacialTracking::GetFacialData(PxrFTInfo **faceTrackingData, pxr_eyepose_data_v2_0 **eyeTrackingData)
{
    if (this->faceTrackingDataBuffer == nullptr || this->eyeTrackingDataBuffer == nullptr)
        return false;

    PxrFTInfo *face = static_cast<PxrFTInfo *>(this->faceTrackingDataBuffer->GetLatest());
    pxr_eyepose_data_v2_0 *eye = static_cast<pxr_eyepose_data_v2_0 *>(this->eyeTrackingDataBuffer->GetLatest());

    if (face != nullptr)
        this->lastFaceTrackingData = face;
    if (eye != nullptr)
        this->lastEyeTrackingData = eye;

    // Send as soon as either stream has a new sample, with the latest sample of the other one.
    if ((face == nullptr && eye == nullptr) || this->lastFaceTrackingData == nullptr || this->lastEyeTrackingData == nullptr)
        return false;

    *faceTrackingData = this->lastFaceTrackingData;
    *eyeTrackingData = this->lastEyeTrackingData;

    return true;
}
