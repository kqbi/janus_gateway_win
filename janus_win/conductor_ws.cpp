#include "conductor_ws.h"

#include <memory>
#include <utility>
#include <vector>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "defaults.h"
#include "media/engine/webrtcvideocapturerfactory.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/checks.h"
#include "rtc_base/json.h"
#include "rtc_base/logging.h"

// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// Names used for a SessionDescription JSON object.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";
const char kJanusOptName[] = "janus";

class DummySetSessionDescriptionObserver
	: public webrtc::SetSessionDescriptionObserver {
public:
	static DummySetSessionDescriptionObserver* Create() {
		return new rtc::RefCountedObject<DummySetSessionDescriptionObserver>();
	}
	virtual void OnSuccess() { RTC_LOG(INFO) << __FUNCTION__; }
	virtual void OnFailure(webrtc::RTCError error) {
		RTC_LOG(INFO) << __FUNCTION__ << " " << ToString(error.type()) << ": "
			<< error.message();
	}
};

ConductorWs::ConductorWs(PeerConnectionWsClient* client, MainWindow* main_wnd)
	: peer_id_(-1), loopback_(false), client_(client), main_wnd_(main_wnd) {
	client_->RegisterObserver(this);
	main_wnd->RegisterObserver(this);
}

ConductorWs::~ConductorWs() {
	RTC_DCHECK(!peer_connection_);
}

bool ConductorWs::connection_active() const {
	return peer_connection_ != nullptr;
}

void ConductorWs::Close() {
	client_->SignOut();
	DeletePeerConnection();
}

bool ConductorWs::InitializePeerConnection() {
	RTC_DCHECK(!peer_connection_factory_);
	RTC_DCHECK(!peer_connection_);

	peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
		nullptr /* network_thread */, nullptr /* worker_thread */,
		nullptr /* signaling_thread */, nullptr /* default_adm */,
		webrtc::CreateBuiltinAudioEncoderFactory(),
		webrtc::CreateBuiltinAudioDecoderFactory(),
		webrtc::CreateBuiltinVideoEncoderFactory(),
		webrtc::CreateBuiltinVideoDecoderFactory(), nullptr /* audio_mixer */,
		nullptr /* audio_processing */);

	if (!peer_connection_factory_) {
		main_wnd_->MessageBox("Error", "Failed to initialize PeerConnectionFactory",
			true);
		DeletePeerConnection();
		return false;
	}

	if (!CreatePeerConnection(/*dtls=*/true)) {
		main_wnd_->MessageBox("Error", "CreatePeerConnection failed", true);
		DeletePeerConnection();
	}

	AddTracks();

	return peer_connection_ != nullptr;
}

bool ConductorWs::ReinitializePeerConnectionForLoopback() {
	loopback_ = true;
	std::vector<rtc::scoped_refptr<webrtc::RtpSenderInterface>> senders =
		peer_connection_->GetSenders();
	peer_connection_ = nullptr;
	if (CreatePeerConnection(/*dtls=*/false)) {
		for (const auto& sender : senders) {
			peer_connection_->AddTrack(sender->track(), sender->stream_ids());
		}
		peer_connection_->CreateOffer(
			this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
	}
	return peer_connection_ != nullptr;
}

bool ConductorWs::CreatePeerConnection(bool dtls) {
	RTC_DCHECK(peer_connection_factory_);
	RTC_DCHECK(!peer_connection_);

	webrtc::PeerConnectionInterface::RTCConfiguration config;
	config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
	config.enable_dtls_srtp = dtls;
	webrtc::PeerConnectionInterface::IceServer server;
	server.uri = GetPeerConnectionString();
	config.servers.push_back(server);

	peer_connection_ = peer_connection_factory_->CreatePeerConnection(
		config, nullptr, nullptr, this);
	return peer_connection_ != nullptr;
}

void ConductorWs::DeletePeerConnection() {
	main_wnd_->StopLocalRenderer();
	main_wnd_->StopRemoteRenderer();
	peer_connection_ = nullptr;
	peer_connection_factory_ = nullptr;
	peer_id_ = -1;
	loopback_ = false;
}

void ConductorWs::EnsureStreamingUI() {
	RTC_DCHECK(peer_connection_);
	if (main_wnd_->IsWindow()) {
		if (main_wnd_->current_ui() != MainWindow::STREAMING)
			main_wnd_->SwitchToStreamingUI();
	}
}

//
// PeerConnectionObserver implementation.
//

void ConductorWs::OnAddTrack(
	rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
	const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
	streams) {
	RTC_LOG(INFO) << __FUNCTION__ << " " << receiver->id();
	main_wnd_->QueueUIThreadCallback(NEW_TRACK_ADDED,
		receiver->track().release());
}

void ConductorWs::OnRemoveTrack(
	rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
	RTC_LOG(INFO) << __FUNCTION__ << " " << receiver->id();
	main_wnd_->QueueUIThreadCallback(TRACK_REMOVED, receiver->track().release());
}

void ConductorWs::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
	RTC_LOG(INFO) << __FUNCTION__ << " " << candidate->sdp_mline_index();
	// For loopback test. To save some connecting delay.
	if (loopback_) {
		if (!peer_connection_->AddIceCandidate(candidate)) {
			RTC_LOG(WARNING) << "Failed to apply the received candidate";
		}
		return;
	}

	Json::StyledWriter writer;
	Json::Value jmessage;

	jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
	jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
	std::string sdp;
	if (!candidate->ToString(&sdp)) {
		RTC_LOG(LS_ERROR) << "Failed to serialize candidate";
		return;
	}
	jmessage[kCandidateSdpName] = sdp;
	SendMessage(writer.write(jmessage));
}

//
// PeerConnectionClientObserver implementation.
//

void ConductorWs::OnSignedIn() {
	RTC_LOG(INFO) << __FUNCTION__;
	main_wnd_->SwitchToPeerList(client_->peers());
}

void ConductorWs::OnDisconnected() {
	RTC_LOG(INFO) << __FUNCTION__;

	DeletePeerConnection();

	if (main_wnd_->IsWindow())
		main_wnd_->SwitchToConnectUI();
}

void ConductorWs::OnPeerConnected(int id, const std::string& name) {
	RTC_LOG(INFO) << __FUNCTION__;
	// Refresh the list if we're showing it.
	if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
		main_wnd_->SwitchToPeerList(client_->peers());
}

void ConductorWs::OnPeerDisconnected(int id) {
	RTC_LOG(INFO) << __FUNCTION__;
	if (id == peer_id_) {
		RTC_LOG(INFO) << "Our peer disconnected";
		main_wnd_->QueueUIThreadCallback(PEER_CONNECTION_CLOSED, NULL);
	}
	else {
		// Refresh the list if we're showing it.
		if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
			main_wnd_->SwitchToPeerList(client_->peers());
	}
}

//because janus self act as an end,so always define peer_id=0
void ConductorWs::OnMessageFromJanus(int peer_id, const std::string& message) {
	RTC_DCHECK(!message.empty());
	//parse json here
	std::cout << "Got wsmsg:"<<message;
	//TODO make sure in right state
	//parse json
	Json::Reader reader;
	Json::Value jmessage;
	if (!reader.parse(message, jmessage)) {
		RTC_LOG(WARNING) << "Received unknown message. " << message;
		return;
	}
	std::string janus_str;
	std::string json_object;

	rtc::GetStringFromJsonObject(jmessage, "janus",
		&janus_str);
	if (!janus_str.empty()) {
		if (janus_str == "ack") {
			// Just an ack, we can probably ignore
			RTC_LOG(INFO) << "Got an ack on session. ";
		}
		else if (janus_str == "success") {
			rtc::GetStringFromJsonObject(jmessage, "transaction",
				&janus_str);
			std::shared_ptr<JanusTransaction> jt = m_transactionMap.at(janus_str);
			//call signal
			if (jt) {
				jt->Success(0, message);//handle_id not ready yet
			}
			m_transactionMap.erase(janus_str);
		}
		else if (janus_str=="trickle") {
			RTC_LOG(INFO) << "Got a trickle candidate from Janus. ";
		}
		else if (janus_str == "webrtcup") {
			RTC_LOG(INFO) << "The PeerConnection with the gateway is up!";
		}
		else if (janus_str == "hangup") {
			RTC_LOG(INFO) << "A plugin asked the core to hangup a PeerConnection on one of our handles! ";
		}
		else if (janus_str == "detached") {
			RTC_LOG(INFO) << "A plugin asked the core to detach one of our handles! ";
		}
		else if (janus_str == "media") {
			RTC_LOG(INFO) << "Media started/stopped flowing. ";
		}
		else if (janus_str == "slowlink") {
			RTC_LOG(INFO) << "Got a slowlink event! ";
		}
		else if (janus_str == "error") {
			RTC_LOG(INFO) << "Got an error. ";
			// Oops, something wrong happened
			rtc::GetStringFromJsonObject(jmessage, "transaction",
				&janus_str);
			std::shared_ptr<JanusTransaction> jt = m_transactionMap.at(janus_str);
			//call signal
			if (jt) {
				jt->Error("123","456");//TODO need to parse the error code and desc
			}
			m_transactionMap.erase(janus_str);
		}
		else {

			if (janus_str == "event") {
				RTC_LOG(INFO) << "Got a plugin event! ";

				rtc::GetStringFromJsonObject(jmessage, "transaction",
					&janus_str);
				std::shared_ptr<JanusTransaction> jt = m_transactionMap.at(janus_str);
				if (jt) {
					jt->Event(message);
				}

			}
		}
	}
}

void ConductorWs::OnMessageSent(int err) {
	// Process the next pending message if any.
	main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, NULL);
}

void ConductorWs::OnServerConnectionFailure() {
	main_wnd_->MessageBox("Error", ("Failed to connect to " + server_).c_str(),
		true);
}

//
// MainWndCallback implementation.
//

void ConductorWs::StartLogin(const std::string& server, int port) {
	if (client_->is_connected())
		return;
	server_ = server;
	auto ws_server = std::string("ws://") + server + std::string(":") + std::to_string(port);
	client_->Connect(ws_server, "1111");
}

void ConductorWs::DisconnectFromServer() {
	if (client_->is_connected())
		client_->SignOut();
}

void ConductorWs::ConnectToPeer(int peer_id) {
	RTC_DCHECK(peer_id_ == -1);
	RTC_DCHECK(peer_id != -1);

	if (peer_connection_.get()) {
		main_wnd_->MessageBox(
			"Error", "We only support connecting to one peer at a time", true);
		return;
	}

	if (InitializePeerConnection()) {
		peer_id_ = peer_id;
		peer_connection_->CreateOffer(
			this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
	}
	else {
		main_wnd_->MessageBox("Error", "Failed to initialize PeerConnection", true);
	}
}

std::unique_ptr<cricket::VideoCapturer> ConductorWs::OpenVideoCaptureDevice() {
	std::vector<std::string> device_names;
	{
		std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
			webrtc::VideoCaptureFactory::CreateDeviceInfo());
		if (!info) {
			return nullptr;
		}
		int num_devices = info->NumberOfDevices();
		for (int i = 0; i < num_devices; ++i) {
			const uint32_t kSize = 256;
			char name[kSize] = { 0 };
			char id[kSize] = { 0 };
			if (info->GetDeviceName(i, name, kSize, id, kSize) != -1) {
				device_names.push_back(name);
			}
		}
	}

	cricket::WebRtcVideoDeviceCapturerFactory factory;
	std::unique_ptr<cricket::VideoCapturer> capturer;
	for (const auto& name : device_names) {
		capturer = factory.Create(cricket::Device(name, 0));
		if (capturer) {
			break;
		}
	}
	return capturer;
}

void ConductorWs::AddTracks() {
	if (!peer_connection_->GetSenders().empty()) {
		return;  // Already added tracks.
	}

	rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
		peer_connection_factory_->CreateAudioTrack(
			kAudioLabel, peer_connection_factory_->CreateAudioSource(
				cricket::AudioOptions())));
	auto result_or_error = peer_connection_->AddTrack(audio_track, { kStreamId });
	if (!result_or_error.ok()) {
		RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
			<< result_or_error.error().message();
	}

	std::unique_ptr<cricket::VideoCapturer> video_device =
		OpenVideoCaptureDevice();
	if (video_device) {
		rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_(
			peer_connection_factory_->CreateVideoTrack(
				kVideoLabel, peer_connection_factory_->CreateVideoSource(
					std::move(video_device), nullptr)));
		main_wnd_->StartLocalRenderer(video_track_);

		result_or_error = peer_connection_->AddTrack(video_track_, { kStreamId });
		if (!result_or_error.ok()) {
			RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
				<< result_or_error.error().message();
		}
	}
	else {
		RTC_LOG(LS_ERROR) << "OpenVideoCaptureDevice failed";
	}

	main_wnd_->SwitchToStreamingUI();
}

void ConductorWs::DisconnectFromCurrentPeer() {
	RTC_LOG(INFO) << __FUNCTION__;
	if (peer_connection_.get()) {
		client_->SendHangUp(peer_id_);
		DeletePeerConnection();
	}

	if (main_wnd_->IsWindow())
		main_wnd_->SwitchToPeerList(client_->peers());
}

void ConductorWs::UIThreadCallback(int msg_id, void* data) {
	switch (msg_id) {
	case PEER_CONNECTION_CLOSED:
		RTC_LOG(INFO) << "PEER_CONNECTION_CLOSED";
		DeletePeerConnection();

		if (main_wnd_->IsWindow()) {
			if (client_->is_connected()) {
				main_wnd_->SwitchToPeerList(client_->peers());
			}
			else {
				main_wnd_->SwitchToConnectUI();
			}
		}
		else {
			DisconnectFromServer();
		}
		break;

	case SEND_MESSAGE_TO_PEER: {
		RTC_LOG(INFO) << "SEND_MESSAGE_TO_PEER";
		std::string* msg = reinterpret_cast<std::string*>(data);
		if (msg) {
			// For convenience, we always run the message through the queue.
			// This way we can be sure that messages are sent to the server
			// in the same order they were signaled without much hassle.
			pending_messages_.push_back(msg);
		}

		if (!pending_messages_.empty() && !client_->IsSendingMessage()) {
			msg = pending_messages_.front();
			pending_messages_.pop_front();
			client_->SendToJanus(*msg);
			delete msg;
		}
		break;
	}

	case NEW_TRACK_ADDED: {
		auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
		if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
			auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
			main_wnd_->StartRemoteRenderer(video_track);
		}
		track->Release();
		break;
	}

	case TRACK_REMOVED: {
		// Remote peer stopped sending a track.
		auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
		track->Release();
		break;
	}

	default:
		RTC_NOTREACHED();
		break;
	}
}

//override CreateSessionDescriptionObserver::OnSuccess
void ConductorWs::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
	peer_connection_->SetLocalDescription(
		DummySetSessionDescriptionObserver::Create(), desc);

	std::string sdp;
	desc->ToString(&sdp);
	SendOffer(m_HandleId, webrtc::SdpTypeToString(desc->GetType()), sdp);

	

	Json::StyledWriter writer;
	Json::Value jmessage;
	jmessage[kSessionDescriptionTypeName] =
		webrtc::SdpTypeToString(desc->GetType());
	jmessage[kSessionDescriptionSdpName] = sdp;
	SendMessage(writer.write(jmessage));
}

void ConductorWs::OnFailure(webrtc::RTCError error) {
	RTC_LOG(LERROR) << ToString(error.type()) << ": " << error.message();
}

void ConductorWs::SendMessage(const std::string& json_object) {
	std::string* msg = new std::string(json_object);
	main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, msg);
}

void ConductorWs::OnJanusConnected() {
	CreateSession();
}

void ConductorWs::CreateSession() {
	std::string transactionID=RandomString(12);
	std::shared_ptr<JanusTransaction> jt(new JanusTransaction());
	jt->transactionId = transactionID;

	//TODO Is it possible for lamda expression here?
	jt->Success = [=](int handle_id,std::string message) mutable {
		//parse json
		Json::Reader reader;
		Json::Value jmessage;
		if (!reader.parse(message, jmessage)) {
			RTC_LOG(WARNING) << "Received unknown message. " << message;
			return;
		}
		std::string janus_str;
		std::string json_object;
		Json::Value janus_value;
		Json::Value janus_session_value;

		rtc::GetValueFromJsonObject(jmessage, "data",
			&janus_value);
		rtc::GetValueFromJsonObject(janus_value, "id",
			&janus_session_value);
		std::string sessionId_str=rtc::JsonValueToString(janus_session_value);
		m_SessionId = std::stoll(sessionId_str);
		//lauch the timer for keep alive breakheart
		//Then Create the handle
		CreateHandle();
	};

	jt->Error = [=](std::string code, std::string reason) {
		RTC_LOG(INFO) << "Ooops: " << code << " " << reason;
	};

	m_transactionMap[transactionID]=jt;

	Json::StyledWriter writer;
	Json::Value jmessage;
	jmessage["janus"] = "create";
	jmessage["transaction"] = transactionID;
	//SendMessage(writer.write(jmessage));
	client_->SendToJanus(writer.write(jmessage));
}

void ConductorWs::CreateHandle() {
	std::string transactionID = RandomString(12);
	std::shared_ptr<JanusTransaction> jt(new JanusTransaction());
	jt->transactionId = transactionID;
	jt->Success = [=](int handle_id, std::string message) {
		//parse json
		Json::Reader reader;
		Json::Value jmessage;
		if (!reader.parse(message, jmessage)) {
			RTC_LOG(WARNING) << "Received unknown message. " << message;
			return;
		}
		std::string janus_str;
		std::string json_object;
		Json::Value janus_value;
		Json::Value janus_handle_value;

		rtc::GetValueFromJsonObject(jmessage, "data",
			&janus_value);
		rtc::GetValueFromJsonObject(janus_value, "id",
			&janus_handle_value);
		std::string handleId_str = rtc::JsonValueToString(janus_handle_value);
		m_HandleId = std::stoll(handleId_str);

		JoinRoom(m_HandleId, 0);//TODO feedid means nothing in echotest,else?
	};

	jt->Event = [=](std::string message) {
		
	};

	jt->Error = [=](std::string, std::string) {
		RTC_LOG(INFO)<<"CreateHandle error:";
	};

	m_transactionMap[transactionID] = jt;

	Json::StyledWriter writer;
	Json::Value jmessage;
	Json::Value jdata;
	jdata["id"]=
	jmessage["janus"] = "attach";
	jmessage["plugin"] = "janus.plugin.echotest";
	jmessage["transaction"] = transactionID;
	jmessage["session_id"] = m_SessionId;
	client_->SendToJanus(writer.write(jmessage));
}

void ConductorWs::JoinRoom(long long int handleId,long long int feedId) {
	//rtcEvents.onPublisherJoined(handle.handleId);
	std::string transactionID = RandomString(12);
	std::shared_ptr<JanusTransaction> jt(new JanusTransaction());
	jt->transactionId = transactionID;

	jt->Event = [=](std::string message) {
		//parse json
		Json::Reader reader;
		Json::Value jmessage;
		if (!reader.parse(message, jmessage)) {
			RTC_LOG(WARNING) << "Received unknown message. " << message;
			return;
		}
		std::string janus_str;
		std::string json_object;
		Json::Value janus_value;
		std::string nego_result;

		rtc::GetValueFromJsonObject(jmessage, "data",
			&janus_value);
		rtc::GetStringFromJsonObject(janus_value, "result",
			&nego_result);
		if (nego_result != "ok") {
			RTC_LOG(WARNING) << "negotiation failed! ";
		}
		//�ж���onjoined����onRemoteJsep,Ȼ����ݶ�Ӧ��handleId���д���
		/*if (onjoined) {
			rtcInterfaces.onPublisherJoined(jh.handleId);
		}
		else if (onRemoteJsep) {
			//rtcEvents.onPublisherRemoteJsep(handle.handleId, jsep);
		}
		else {

		}*/
	};

	m_transactionMap[transactionID] = jt;

	Json::StyledWriter writer;
	Json::Value jmessage;
	Json::Value jbody;
	jbody["audio"] = true;
	jbody["video"] = true;
	jmessage["body"] = jbody;
	jmessage["janus"] = "message";
	jmessage["transaction"] = transactionID;
	jmessage["session_id"] = m_SessionId;
	jmessage["handle_id"] = m_HandleId;
	client_->SendToJanus(writer.write(jmessage));
	if (InitializePeerConnection()) {
		peer_connection_->CreateOffer(
			this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
	}
	else {
		main_wnd_->MessageBox("Error", "Failed to initialize PeerConnection", true);
	}
}

void ConductorWs::SendOffer(long long int handleId, std::string sdp_type,std::string sdp_desc) {
	std::string transactionID = RandomString(12);
	std::shared_ptr<JanusTransaction> jt(new JanusTransaction());
	jt->transactionId = transactionID;

	jt->Event = [=](std::string message) {
		//parse json
		Json::Reader reader;
		Json::Value jmessage;
		if (!reader.parse(message, jmessage)) {
			RTC_LOG(WARNING) << "Received unknown message. " << message;
			return;
		}
		std::string janus_str;
		std::string json_object;
		Json::Value janus_value;
		std::string jsep_str;
		//see if has remote jsep
		rtc::GetValueFromJsonObject(janus_value, "jsep",
			&janus_value);
		if (janus_value!=NULL) {
			//should set remote sdp
			std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
				webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, rtc::JsonValueToString(janus_value));
			peer_connection_->SetRemoteDescription(
				DummySetSessionDescriptionObserver::Create(),
				session_description.release());
		}	
	};

	m_transactionMap[transactionID] = jt;

	Json::StyledWriter writer;
	Json::Value jmessage;
	Json::Value jbody;
	Json::Value jjsep;

	jbody["request"] = "configure";
	jbody["audio"] = true;
	jbody["video"] = true;

	jjsep["type"] = sdp_type;
	jjsep["sdp"] = sdp_desc;

	jmessage["body"] = jbody;
	jmessage["jsep"] = jjsep;
	jmessage["janus"] = "message";
	jmessage["transaction"] = transactionID;
	jmessage["session_id"] = m_SessionId;
	jmessage["handle_id"] = m_HandleId;
	client_->SendToJanus(writer.write(jmessage));
}

void ConductorWs::trickleCandidate(long long int handleId, const webrtc::IceCandidateInterface* candidate) {
	std::string transactionID = RandomString(12);
	Json::StyledWriter writer;
	Json::Value jmessage;
	Json::Value jcandidate;

	std::string sdp;
	if (!candidate->ToString(&sdp)) {
		RTC_LOG(LS_ERROR) << "Failed to serialize candidate";
		return;
	}

	jcandidate["sdpMid"]= candidate->sdp_mid();
	jcandidate["sdpMLineIndex"] = candidate->sdp_mline_index();
	jcandidate["sdpMid"] = sdp;

	jmessage["janus"] = "message";
	jmessage["candidate"] = jcandidate;	
	jmessage["transaction"] = transactionID;
	jmessage["session_id"] = m_SessionId;
	jmessage["handle_id"] = m_HandleId;
	client_->SendToJanus(writer.write(jmessage));
}

void ConductorWs::trickleCandidateComplete(long long int handleId) {

}

//we have arrived at OnLocalStream and OnRemoteSteam
//thread problem should fix when debug

