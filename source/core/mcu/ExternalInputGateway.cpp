/*
 * Copyright 2015 Intel Corporation All Rights Reserved. 
 * 
 * The source code contained or described herein and all documents related to the 
 * source code ("Material") are owned by Intel Corporation or its suppliers or 
 * licensors. Title to the Material remains with Intel Corporation or its suppliers 
 * and licensors. The Material contains trade secrets and proprietary and 
 * confidential information of Intel or its suppliers and licensors. The Material 
 * is protected by worldwide copyright and trade secret laws and treaty provisions. 
 * No part of the Material may be used, copied, reproduced, modified, published, 
 * uploaded, posted, transmitted, distributed, or disclosed in any way without 
 * Intel's prior express written permission.
 * 
 * No license under any patent, copyright, trade secret or other intellectual 
 * property right is granted to or conferred upon you by disclosure or delivery of 
 * the Materials, either expressly, by implication, inducement, estoppel or 
 * otherwise. Any license under such intellectual property rights must be express 
 * and approved by Intel in writing.
 */

#include "ExternalInputGateway.h"
#include "MediaMuxerFactory.h"
#include "media/ExternalOutput.h"

#include <boost/property_tree/json_parser.hpp>
#include <EncodedVideoFrameSender.h>

using namespace erizo;

namespace mcu {

DEFINE_LOGGER(ExternalInputGateway, "mcu.ExternalInputGateway");

ExternalInputGateway::ExternalInputGateway()
    : m_incomingVideoFormat(woogeen_base::FRAME_FORMAT_UNKNOWN)
    , m_videoOutputKbps(0)
{
    m_taskRunner.reset(new woogeen_base::WebRTCTaskRunner());
    m_audioTranscoder.reset(new AudioMixer(this, nullptr));
    m_taskRunner->Start();
}

ExternalInputGateway::~ExternalInputGateway()
{
    m_taskRunner->Stop();
    closeAll();
}

int ExternalInputGateway::deliverAudioData(char* buf, int len)
{
    if (len <= 0)
        return 0;

    {
        boost::shared_lock<boost::shared_mutex> lock(m_sinkMutex);
        if (audioSink_)
            audioSink_->deliverAudioData(buf, len);
    }

    if (!m_subscribers.empty())
        return m_audioTranscoder->deliverAudioData(buf, len);

    return 0;
}

int ExternalInputGateway::deliverVideoData(char* buf, int len)
{
    if (len <= 0)
        return 0;

    {
        boost::shared_lock<boost::shared_mutex> lock(m_sinkMutex);
        if (videoSink_)
            videoSink_->deliverVideoData(buf, len);
    }

    boost::shared_lock<boost::shared_mutex> transcoderLock(m_videoTranscoderMutex);
    std::vector<boost::shared_ptr<woogeen_base::VideoFrameTranscoder>>::iterator it = m_videoTranscoders.begin();
    for (; it != m_videoTranscoders.end(); ++it) {
        woogeen_base::Frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.format = m_incomingVideoFormat;
        frame.payload = reinterpret_cast<uint8_t*>(buf);
        frame.length = len;
        (*it)->onFrame(frame);
    }

    return len;
}

int ExternalInputGateway::deliverFeedback(char* buf, int len)
{
    // TODO: For now we just send the feedback to all of the output processors.
    // The output processor will filter out the feedback which does not belong
    // to it. In the future we may do the filtering at a higher level?
    boost::shared_lock<boost::shared_mutex> lock(m_videoOutputMutex);
    std::map<int, boost::shared_ptr<woogeen_base::VideoFrameSender>>::iterator it = m_videoOutputs.begin();
    for (; it != m_videoOutputs.end(); ++it) {
        FeedbackSink* feedbackSink = it->second->feedbackSink();
        if (feedbackSink)
            feedbackSink->deliverFeedback(buf, len);
    }

    m_audioTranscoder->deliverFeedback(buf, len);

    return len;
}

void ExternalInputGateway::receiveRtpData(char* buf, int len, DataType type, uint32_t streamId)
{
    if (m_subscribers.empty() || len <= 0)
        return;

    uint32_t ssrc = 0;
    RTCPHeader* chead = reinterpret_cast<RTCPHeader*>(buf);
    uint8_t packetType = chead->getPacketType();
    assert(packetType != RTCP_Receiver_PT && packetType != RTCP_PS_Feedback_PT && packetType != RTCP_RTP_Feedback_PT);
    if (packetType == RTCP_Sender_PT)
        ssrc = chead->getSSRC();
    else {
        RTPHeader* head = reinterpret_cast<RTPHeader*>(buf);
        ssrc = head->getSSRC();
    }

    std::map<std::string, boost::shared_ptr<MediaSink>>::iterator it;
    boost::shared_lock<boost::shared_mutex> lock(m_subscriberMutex);
    switch (type) {
    case erizo::AUDIO: {
        for (it = m_subscribers.begin(); it != m_subscribers.end(); ++it) {
            MediaSink* sink = it->second.get();
            if (sink && sink->getAudioSinkSSRC() == ssrc)
                sink->deliverAudioData(buf, len);
        }
        break;
    }
    case erizo::VIDEO: {
        for (it = m_subscribers.begin(); it != m_subscribers.end(); ++it) {
            MediaSink* sink = it->second.get();
            if (sink && sink->getVideoSinkSSRC() == ssrc)
                sink->deliverVideoData(buf, len);
        }
        break;
    }
    default:
        break;
    }
}

bool ExternalInputGateway::addPublisher(MediaSource* publisher, const std::string& id)
{
    if (m_publisher) {
        ELOG_WARN("Publisher already exists: %p, id %s, ignoring the new set request (%p, %s)",
            m_publisher.get(), m_participantId.c_str(), publisher, id.c_str());
        return false;
    }

    if (publisher->getVideoDataType() != DataContentType::ENCODED_FRAME) {
        ELOG_ERROR("Wrong kind of publisher: %p, id %s, ignoring)",
             publisher, id.c_str());
        assert(false);
        return false;
    }

    switch (publisher->getVideoPayloadType()) {
    case VP8_90000_PT:
        m_incomingVideoFormat = woogeen_base::FRAME_FORMAT_VP8;
        break;
    case H264_90000_PT:
        m_incomingVideoFormat = woogeen_base::FRAME_FORMAT_H264;
        break;
    default:
        break;
    }

    erizo::FeedbackSink* feedbackSink = publisher->getFeedbackSink();
    uint32_t audioSSRC = publisher->getAudioSourceSSRC();

    if (audioSSRC)
        m_audioTranscoder->addSource(audioSSRC, true, publisher->getAudioDataType(), publisher->getAudioPayloadType(), feedbackSink, id);

    videoSourceSSRC_ = publisher->getVideoSourceSSRC();
    audioSourceSSRC_ = audioSSRC;
    sourcefbSink_ = feedbackSink;
    videoDataType_ = publisher->getVideoDataType();
    audioDataType_ = publisher->getAudioDataType();
    videoPayloadType_ = publisher->getVideoPayloadType();
    audioPayloadType_ = publisher->getAudioPayloadType();

    publisher->setAudioSink(this);
    publisher->setVideoSink(this);

    m_participantId = id;
    m_publisher.reset(publisher);

    return true;
}

void ExternalInputGateway::removePublisher(const std::string& id)
{
    if (!m_publisher || id != m_participantId) {
        ELOG_WARN("Publisher doesn't exist; can't unset the publisher");
        return;
    }

    if (!m_subscribers.empty()) {
        ELOG_WARN("There're still %zu subscribers to current publisher; can't unset the publisher", m_subscribers.size());
        return;
    }

    {
        boost::shared_lock<boost::shared_mutex> transcoderLock(m_videoTranscoderMutex);
        std::vector<boost::shared_ptr<woogeen_base::VideoFrameTranscoder>>::iterator it = m_videoTranscoders.begin();
        for (; it != m_videoTranscoders.end(); ++it)
            (*it)->unsetInput();
    }

    m_audioTranscoder->removeSource(m_publisher->getAudioSourceSSRC(), true);

    m_publisher.reset();
}

void ExternalInputGateway::addSubscriber(MediaSink* subscriber, const std::string& id)
{
    int videoPayloadType = subscriber->preferredVideoPayloadType();
    // FIXME: Now we hard code the output to be NACK enabled and FEC disabled,
    // because the video mixer now is not able to output different formatted
    // RTP packets for a single encoded stream elegantly.
    bool enableNACK = true || subscriber->acceptResentData();
    bool enableFEC = false && subscriber->acceptFEC();
    uint32_t videoSSRC = addVideoOutput(videoPayloadType, enableNACK, enableFEC);
    subscriber->setVideoSinkSSRC(videoSSRC);

    int audioPayloadType = subscriber->preferredAudioPayloadType();
    int32_t channelId = m_audioTranscoder->addOutput(id, audioPayloadType);
    subscriber->setAudioSinkSSRC(m_audioTranscoder->getSendSSRC(channelId));

    ELOG_DEBUG("Adding subscriber to %u(a), %u(v)", m_audioTranscoder->getSendSSRC(channelId), videoSSRC);

    FeedbackSource* fbsource = subscriber->getFeedbackSource();
    if (fbsource) {
        ELOG_DEBUG("adding fbsource");
        fbsource->setFeedbackSink(this);
    }

    boost::unique_lock<boost::shared_mutex> lock(m_subscriberMutex);
    m_subscribers[id] = boost::shared_ptr<MediaSink>(subscriber);
}

void ExternalInputGateway::removeSubscriber(const std::string& id)
{
    ELOG_DEBUG("Removing subscriber: id is %s", id.c_str());

    std::vector<boost::shared_ptr<MediaSink>> removedSubscribers;
    boost::unique_lock<boost::shared_mutex> lock(m_subscriberMutex);
    std::map<std::string, boost::shared_ptr<MediaSink>>::iterator it = m_subscribers.find(id);
    if (it != m_subscribers.end()) {
        boost::shared_ptr<MediaSink>& subscriber = it->second;
        if (subscriber) {
            FeedbackSource* fbsource = subscriber->getFeedbackSource();
            if (fbsource)
                fbsource->setFeedbackSink(nullptr);
            removedSubscribers.push_back(subscriber);
        }
        m_subscribers.erase(it);
    }
    lock.unlock();
}

void ExternalInputGateway::setAudioSink(MediaSink* sink)
{
    boost::unique_lock<boost::shared_mutex> lock(m_sinkMutex);
    audioSink_ = sink;
}

void ExternalInputGateway::setVideoSink(MediaSink* sink)
{
    boost::unique_lock<boost::shared_mutex> lock(m_sinkMutex);
    videoSink_ = sink;
}

int ExternalInputGateway::sendFirPacket()
{
    return m_publisher ? m_publisher->sendFirPacket() : -1;
}

int ExternalInputGateway::setVideoCodec(const std::string& codecName, unsigned int clockRate)
{
    return m_publisher ? m_publisher->setVideoCodec(codecName, clockRate) : -1;
}

int ExternalInputGateway::setAudioCodec(const std::string& codecName, unsigned int clockRate)
{
    return m_publisher ? m_publisher->setAudioCodec(codecName, clockRate) : -1;
}

bool ExternalInputGateway::addExternalOutput(const std::string& configParam, woogeen_base::EventRegistry* callback)
{
    // Create an ExternalOutput here
    if (configParam != "" && configParam != "undefined") {
        boost::property_tree::ptree pt;
        std::istringstream is(configParam);
        boost::property_tree::read_json(is, pt);
        const std::string outputId = pt.get<std::string>("id", "");

        std::map<std::string, boost::shared_ptr<erizo::MediaSink>>::iterator it = m_subscribers.find(outputId);
        if (it == m_subscribers.end()) {
            woogeen_base::MediaMuxer* muxer = MediaMuxerFactory::createMediaMuxer(outputId, configParam, callback);
            if (muxer) {
                // Create an external output, which will be managed as subscriber during its lifetime
                ExternalOutput* externalOutput = new ExternalOutput(muxer);

                // Added as a subscriber
                addSubscriber(externalOutput, outputId);

                return true;
            }
        } else {
            ELOG_DEBUG("External output with id %s has already been occupied.", outputId.c_str());
        }
    }

    return false;
}

bool ExternalInputGateway::removeExternalOutput(const std::string& outputId, bool close)
{
    // Remove the external output
    removeSubscriber(outputId);

    if (close)
        return MediaMuxerFactory::recycleMediaMuxer(outputId); // Remove the media muxer

    return true;
}

void ExternalInputGateway::closeAll()
{
    ELOG_DEBUG("closeAll");

    std::vector<boost::shared_ptr<MediaSink>> removedSubscribers;
    boost::unique_lock<boost::shared_mutex> subscriberLock(m_subscriberMutex);
    std::map<std::string, boost::shared_ptr<MediaSink>>::iterator subscriberItor = m_subscribers.begin();
    while (subscriberItor != m_subscribers.end()) {
        boost::shared_ptr<MediaSink>& subscriber = subscriberItor->second;
        if (subscriber) {
            FeedbackSource* fbsource = subscriber->getFeedbackSource();
            if (fbsource)
                fbsource->setFeedbackSink(nullptr);
            removedSubscribers.push_back(subscriber);
        }
        m_subscribers.erase(subscriberItor++);
    }
    m_subscribers.clear();
    subscriberLock.unlock();

    removePublisher(m_participantId);
}

static const int OUTPUT_VP8_VIDEO_STREAM_ID = 0;
static const int OUTPUT_H264_VIDEO_STREAM_ID = 1;

uint32_t ExternalInputGateway::addVideoOutput(int payloadType, bool nack, bool fec)
{
    boost::upgrade_lock<boost::shared_mutex> lock(m_videoOutputMutex);
    std::map<int, boost::shared_ptr<woogeen_base::VideoFrameSender>>::iterator it = m_videoOutputs.find(payloadType);
    if (it != m_videoOutputs.end()) {
        it->second->startSend(nack, fec);
        return it->second->sendSSRC(nack, fec);
    }

    woogeen_base::FrameFormat outputFormat = woogeen_base::FRAME_FORMAT_UNKNOWN;
    int outputId = -1;
    switch (payloadType) {
    case VP8_90000_PT:
        outputFormat = woogeen_base::FRAME_FORMAT_VP8;
        outputId = OUTPUT_VP8_VIDEO_STREAM_ID;
        break;
    case H264_90000_PT:
        outputFormat = woogeen_base::FRAME_FORMAT_H264;
        outputId = OUTPUT_H264_VIDEO_STREAM_ID;
        break;
    default:
        return 0;
    }

    // TODO: Create the transcoders on-demand, i.e., when
    // there're subscribers which don't support the incoming codec.
    // In such case we should use the publisher as the frame provider of the VideoFrameSender,
    // but currently the publisher doesn't implement the VideoFrameProvider interface.
    boost::shared_ptr<woogeen_base::VideoFrameTranscoder> videoTranscoder(new woogeen_base::VideoFrameTranscoder(m_taskRunner));
    videoTranscoder->setInput(m_incomingVideoFormat, nullptr);

    woogeen_base::WebRTCTransport<erizo::VIDEO>* transport = new woogeen_base::WebRTCTransport<erizo::VIDEO>(this, nullptr);
    woogeen_base::VideoFrameSender* output = new woogeen_base::EncodedVideoFrameSender(outputId, videoTranscoder, outputFormat, m_videoOutputKbps, transport, m_taskRunner);
    videoTranscoder->activateOutput(outputId, outputFormat, 30, 500, output);

    {
        boost::unique_lock<boost::shared_mutex> transcoderLock(m_videoTranscoderMutex);
        m_videoTranscoders.push_back(videoTranscoder);
    }

    // Fetch video size.
    // TODO: The size should be identical to the published video size.
    output->setSendCodec(outputFormat, 1280, 720);
    output->startSend(nack, fec);
    boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
    m_videoOutputs[payloadType].reset(output);

    return output->sendSSRC(nack, fec);
}

}/* namespace mcu */