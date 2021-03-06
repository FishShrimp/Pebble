/*
 * Tencent is pleased to support the open source community by making Pebble available.
 * Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.
 * Licensed under the MIT License (the "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing permissions and limitations under
 * the License.
 *
 */

#include <sstream>
#include <string.h>

#include "common/timer.h"
#include "common/time_utility.h"
#include "framework/message.h"
#include "framework/rpc.h"

namespace pebble {

class RpcErrorStringRegister {
public:
    RpcErrorStringRegister() {
        SetErrorString(kRPC_INVALID_PARAM, "invalid paramater");
        SetErrorString(kRPC_ENCODE_FAILED, "encode failed");
        SetErrorString(kRPC_DECODE_FAILED, "decode failed");
        SetErrorString(kRPC_RECV_EXCEPTION_MSG, "receive a exception message");
        SetErrorString(kRPC_UNKNOWN_TYPE, "unknown message type received");
        SetErrorString(kRPC_UNSUPPORT_FUNCTION_NAME, "unsupport function name");
        SetErrorString(kRPC_SESSION_NOT_FOUND, "session is expired");
        SetErrorString(kRPC_SEND_FAILED, "send failed");
        SetErrorString(kRPC_REQUEST_TIMEOUT, "request timeout");
        SetErrorString(kRPC_FUNCTION_NAME_EXISTED, "service name is already registered");
        SetErrorString(kRPC_SYSTEM_ERROR, "system error");
        SetErrorString(kRPC_PROCESS_TIMEOUT, "process service timeout");
        SetErrorString(kPRC_BROADCAST_FAILED, "broadcast request failed");
        SetErrorString(kRPC_FUNCTION_NAME_UNEXISTED, "service name unexisted");

        SetErrorString(kRPC_MESSAGE_EXPIRED, "system overload: message expired");
        SetErrorString(kRPC_TASK_OVERLOAD, "system overload: task overload");
    }
};
static RpcErrorStringRegister s_rpc_error_string_register;


/// @brief RPC会话数据结构定义
struct RpcSession {
    RpcSession() {
        m_session_id  = 0;
        m_handle      = 0;
        m_timerid     = -1;
        m_start_time  = 0;
        m_server_side = false;
    }

    uint64_t m_session_id;
    int64_t  m_handle;
    int64_t  m_timerid;
    int64_t  m_start_time;
    RpcHead  m_rpc_head;
    bool     m_server_side;
    OnRpcResponse m_rsp;
};


Rpc::Rpc() {
    m_session_id        = 0;
    m_timer             = new SequenceTimer();
    m_last_error[0]     = 0;
    m_rpc_event_handler = NULL;
    m_task_num          = 0;
    m_latest_handle     = -1;
}

Rpc::~Rpc() {
    if (m_timer) {
        delete m_timer;
        m_timer = NULL;
    }
}

int32_t Rpc::Update() {
    int32_t num = 0;
    if (m_timer) {
        num += m_timer->Update();
    }

    return num;
}

int32_t Rpc::SetSendFunction(const SendFunction& send, const SendVFunction& sendv) {
    if (!send || !sendv) {
        _LOG_LAST_ERROR("param invalid: !send = %d, !sendv = %d", !send, !sendv);
        return kRPC_INVALID_PARAM;
    }
    m_send  = send;
    m_sendv = sendv;
    return 0;
}

int32_t Rpc::SetBroadcastFunction(const BroadcastFunction& broadcast,
    const BroadcastVFunction& broadcastv) {
    m_broadcast  = broadcast;
    m_broadcastv = broadcastv;
    return 0;
}

int32_t Rpc::OnMessage(int64_t handle, const uint8_t* buff,
    uint32_t buff_len, uint32_t is_overload) {

    if (NULL == buff || 0 == buff_len) {
        _LOG_LAST_ERROR("param invalid: buff = %p, buff_len = %u", buff, buff_len);
        return kRPC_INVALID_PARAM;
    }

    RpcHead head;

    int32_t head_len = HeadDecode(buff, buff_len, &head);
    if (head_len < 0) {
        _LOG_LAST_ERROR("HeadDecode failed(%d).", head_len);
        return kRPC_DECODE_FAILED;
    }

    if (head_len > static_cast<int32_t>(buff_len)) {
        _LOG_LAST_ERROR("head_len(%d) > buff_len(%u)", head_len, buff_len);
        return kRPC_DECODE_FAILED;
    }

    const uint8_t* data = buff + head_len;
    uint32_t data_len   = buff_len - head_len;

    int32_t ret = kRPC_UNKNOWN_TYPE;
    switch (head.m_message_type) {
        case kRPC_CALL:
            if (is_overload != 0) {
                ret = ResponseException(handle, kRPC_SYSTEM_OVERLOAD_BASE - is_overload, head);
                OnRequestProcComplete(head.m_function_name, kRPC_SYSTEM_OVERLOAD_BASE - is_overload, 0);
                break;
            }
        case kRPC_ONEWAY:
            m_latest_handle = handle;
            ret = ProcessRequest(handle, head, data, data_len);
            break;

        case kRPC_REPLY:
        case kRPC_EXCEPTION:
            ret = ProcessResponse(head, data, data_len);
            break;

        default:
            _LOG_LAST_ERROR("rpc msg type error(%d)", head.m_message_type);
            break;
    }

    return ret;
}

int32_t Rpc::AddOnRequestFunction(const std::string& name, const OnRpcRequest& on_request) {
    if (name.empty() || !on_request) {
        _LOG_LAST_ERROR("param invalid: name = %s, !on_request = %u", name.c_str(), !on_request);
        return kRPC_INVALID_PARAM;
    }

    std::pair<cxx::unordered_map<std::string, OnRpcRequest>::iterator, bool> ret =
        m_service_map.insert(std::pair<std::string, OnRpcRequest>(name, on_request));
    if (false == ret.second) {
        _LOG_LAST_ERROR("the %s is existed", name.c_str());
        return kRPC_FUNCTION_NAME_EXISTED;
    }

    return kRPC_SUCCESS;
}

int32_t Rpc::RemoveOnRequestFunction(const std::string& name) {
    return m_service_map.erase(name) == 1 ? kRPC_SUCCESS : kRPC_FUNCTION_NAME_UNEXISTED;
}

void Rpc::GetResourceUsed(cxx::unordered_map<std::string, int64_t>* resource_info) {
    if (!resource_info) {
        return;
    }
    std::ostringstream timer;
    timer << "Rpc(" << this << "):timer";
    (*resource_info)[timer.str()]   = m_timer->GetTimerNum();

    std::ostringstream session;
    session << "Rpc(" << this << "):session";
    (*resource_info)[session.str()] = m_session_map.size();
    return;
}

int32_t Rpc::SendRequest(int64_t handle,
                    const RpcHead& rpc_head,
                    const uint8_t* buff,
                    uint32_t buff_len,
                    const OnRpcResponse& on_rsp,
                    int32_t timeout_ms) {
    // buff允许为空，长度非0时做非空检查
    if (buff_len != 0 && NULL == buff) {
        _LOG_LAST_ERROR("param invalid: buff = %p, buff_len = %u", buff, buff_len);
        return kRPC_INVALID_PARAM;
    }

    // 发送请求
    int32_t ret = Send(handle, rpc_head, buff, buff_len);
    if (ret != kRPC_SUCCESS) {
        _LOG_LAST_ERROR("send failed(%d)", ret);
        OnResponseProcComplete(rpc_head.m_function_name, kRPC_SEND_FAILED, 0);
        return ret;
    }

    // ONEWAY请求
    if (!on_rsp) {
        OnResponseProcComplete(rpc_head.m_function_name, kRPC_SUCCESS, 0);
        return kRPC_SUCCESS;
    }

    // 保持会话
    cxx::shared_ptr<RpcSession> session(new RpcSession());
    session->m_session_id  = rpc_head.m_session_id;
    session->m_handle      = handle;
    session->m_rsp         = on_rsp;
    session->m_rpc_head    = rpc_head;
    session->m_server_side = false;
    TimeoutCallback cb     = cxx::bind(&Rpc::OnTimeout, this, session->m_session_id);

    if (timeout_ms <= 0) {
        timeout_ms = 10 * 1000;
    }
    session->m_timerid     = m_timer->StartTimer(timeout_ms, cb);
    session->m_start_time  = TimeUtility::GetCurrentMS();

    m_session_map[session->m_session_id] = session;

    return kRPC_SUCCESS;
}

int32_t Rpc::BroadcastRequest(const std::string& name,
                    const RpcHead& rpc_head,
                    const uint8_t* buff,
                    uint32_t buff_len) {
    // buff允许为空，长度非0时做非空检查
    if (buff_len != 0 && NULL == buff) {
        _LOG_LAST_ERROR("param invalid: buff = %p, buff_len = %u", buff, buff_len);
        return kRPC_INVALID_PARAM;
    }

    // 发送请求
    int32_t head_len = HeadEncode(rpc_head, m_rpc_head_buff, sizeof(m_rpc_head_buff));
    if (head_len < 0) {
        _LOG_LAST_ERROR("encode head failed(%d)", head_len);
        return kRPC_ENCODE_FAILED;
    }

    const uint8_t* msg_frag[] = { m_rpc_head_buff, buff     };
    uint32_t msg_frag_len[]   = { head_len       , buff_len };

    int32_t num = m_broadcastv(name, sizeof(msg_frag) / sizeof(*msg_frag), msg_frag, msg_frag_len);

    return num >= 0 ? kRPC_SUCCESS : kPRC_BROADCAST_FAILED;
}

int32_t Rpc::SendResponse(uint64_t session_id, int32_t ret,
    const uint8_t* buff, uint32_t buff_len) {

    cxx::unordered_map< uint64_t, cxx::shared_ptr<RpcSession> >::iterator it =
        m_session_map.find(session_id);
    if (m_session_map.end() == it) {
        _LOG_LAST_ERROR("session %lu not found", session_id);
        return kRPC_SESSION_NOT_FOUND;
    }

    m_timer->StopTimer(it->second->m_timerid);

    int32_t result = kRPC_SUCCESS;
    if (kRPC_SUCCESS == ret) {
        it->second->m_rpc_head.m_message_type = kRPC_REPLY;
        ret = Send(it->second->m_handle, it->second->m_rpc_head, buff, buff_len);
    } else {
        result = ResponseException(it->second->m_handle, ret, it->second->m_rpc_head, buff, buff_len);
    }

    OnRequestProcComplete(it->second->m_rpc_head.m_function_name,
        ret, TimeUtility::GetCurrentMS() - it->second->m_start_time);

    m_session_map.erase(it);
    m_task_num--;

    if (result != kRPC_SUCCESS || ret != kRPC_SUCCESS) {
        _LOG_LAST_ERROR("send failed(%d,%d)", ret, result);
        return kRPC_SEND_FAILED;
    }

    return ret;
}

int32_t Rpc::Send(int64_t handle, const RpcHead& rpc_head,
    const uint8_t* buff, uint32_t buff_len) {
    int32_t head_len = HeadEncode(rpc_head, m_rpc_head_buff, sizeof(m_rpc_head_buff));
    if (head_len < 0) {
        _LOG_LAST_ERROR("encode head failed(%d)", head_len);
        return kRPC_ENCODE_FAILED;
    }

    const uint8_t* msg_frag[] = { m_rpc_head_buff, buff     };
    uint32_t msg_frag_len[]   = { head_len       , buff_len };

    return m_sendv(handle, sizeof(msg_frag)/sizeof(*msg_frag), msg_frag, msg_frag_len, 0);
}

int32_t Rpc::OnTimeout(uint64_t session_id) {
    cxx::unordered_map< uint64_t, cxx::shared_ptr<RpcSession> >::iterator it =
        m_session_map.find(session_id);
    if (m_session_map.end() == it) {
        _LOG_LAST_ERROR("session %lu not found", session_id);
        return kRPC_SESSION_NOT_FOUND;
    }

    // request timeout
    if ((it->second)->m_rsp) {
        (it->second)->m_rsp(kRPC_REQUEST_TIMEOUT, NULL, 0);
        Message::ReportHandleResult(it->second->m_handle, kRPC_REQUEST_TIMEOUT, 0);
    }

    if (it->second->m_server_side) {
        m_task_num--;
        OnRequestProcComplete(it->second->m_rpc_head.m_function_name,
            kRPC_PROCESS_TIMEOUT, TimeUtility::GetCurrentMS() - it->second->m_start_time);
    } else {
        OnResponseProcComplete(it->second->m_rpc_head.m_function_name,
            kRPC_REQUEST_TIMEOUT, TimeUtility::GetCurrentMS() - it->second->m_start_time);
    }

    m_session_map.erase(it);

    return kTIMER_BE_REMOVED;
}

int32_t Rpc::ProcessRequest(int64_t handle, const RpcHead& rpc_head,
    const uint8_t* buff, uint32_t buff_len) {
    return ProcessRequestImp(handle, rpc_head, buff, buff_len);
}

int32_t Rpc::ProcessRequestImp(int64_t handle, const RpcHead& rpc_head,
    const uint8_t* buff, uint32_t buff_len) {

    cxx::unordered_map<std::string, OnRpcRequest>::iterator it =
        m_service_map.find(rpc_head.m_function_name);
    if (m_service_map.end() == it) {
        _LOG_LAST_ERROR("%s's request proc func not found", rpc_head.m_function_name.c_str());
        ResponseException(handle, kRPC_UNSUPPORT_FUNCTION_NAME, rpc_head);
        OnRequestProcComplete(rpc_head.m_function_name, kRPC_UNSUPPORT_FUNCTION_NAME, 0);
        return kRPC_UNSUPPORT_FUNCTION_NAME;
    }

    if (kRPC_ONEWAY == rpc_head.m_message_type) {
        cxx::function<int32_t(int32_t, const uint8_t*, uint32_t)> rsp; // NOLINT
        int32_t ret = (it->second)(buff, buff_len, rsp);
        OnRequestProcComplete(rpc_head.m_function_name, ret, 0);
        return ret;
    }

    // 请求处理也保持会话，方便扩展
    cxx::shared_ptr<RpcSession> session(new RpcSession());
    session->m_session_id  = GenSessionId();
    session->m_handle      = handle;
    session->m_rpc_head    = rpc_head;
    session->m_server_side = true;

    TimeoutCallback cb     = cxx::bind(&Rpc::OnTimeout, this, session->m_session_id);
    session->m_timerid     = m_timer->StartTimer(REQ_PROC_TIMEOUT_MS, cb);
    session->m_start_time  = TimeUtility::GetCurrentMS();

    m_session_map[session->m_session_id] = session;
    m_task_num++;

    cxx::function<int32_t(int32_t, const uint8_t*, uint32_t)> rsp = cxx::bind( // NOLINT
        &Rpc::SendResponse, this, session->m_session_id,
        cxx::placeholders::_1, cxx::placeholders::_2, cxx::placeholders::_3);

    return (it->second)(buff, buff_len, rsp);
}

int32_t Rpc::ProcessResponse(const RpcHead& rpc_head,
    const uint8_t* buff, uint32_t buff_len) {

    cxx::unordered_map< uint64_t, cxx::shared_ptr<RpcSession> >::iterator it =
        m_session_map.find(rpc_head.m_session_id);
    if (m_session_map.end() == it) {
        _LOG_LAST_ERROR("session(%lu) not found, function_name(%s)",
                        rpc_head.m_session_id, rpc_head.m_function_name.c_str());
        return kRPC_SESSION_NOT_FOUND;
    }

    m_timer->StopTimer(it->second->m_timerid);

    int ret = kRPC_SUCCESS;
    const uint8_t* real_buff = buff;
    uint32_t real_buff_len = buff_len;

    RpcException exception;
    if (kRPC_EXCEPTION == rpc_head.m_message_type) {
        int32_t len = ExceptionDecode(buff, buff_len, &exception);
        if (len < 0) {
            _LOG_LAST_ERROR("ExceptionDecode failed(%d)", len);
            ret = kRPC_RECV_EXCEPTION_MSG;
            real_buff = NULL;
            real_buff_len = 0;
        } else {
            ret = exception.m_error_code;
            real_buff = (const uint8_t*)exception.m_message.data();
            real_buff_len = exception.m_message.size();
        }
    }

    if (it->second->m_rsp) {
        ret = it->second->m_rsp(ret, real_buff, real_buff_len);
    }

    int64_t time_cost = TimeUtility::GetCurrentMS() - it->second->m_start_time;
    Message::ReportHandleResult(it->second->m_handle,
        (ret == kRPC_MESSAGE_EXPIRED ? 0 : ret), time_cost);
    OnResponseProcComplete(it->second->m_rpc_head.m_function_name, ret, time_cost);

    m_session_map.erase(it);

    return ret;
}

int32_t Rpc::ResponseException(int64_t handle, int32_t ret, const RpcHead& rpc_head,
    const uint8_t* buff, uint32_t buff_len) {

    (const_cast<RpcHead&>(rpc_head)).m_message_type = kRPC_EXCEPTION;

    RpcException exception;
    exception.m_error_code = ret;
    if (buff_len > 0) {
        exception.m_message.assign((const char*)buff, buff_len);
    }

    int32_t len = ExceptionEncode(exception, m_rpc_exception_buff, sizeof(m_rpc_exception_buff));
    if (len < 0) {
        _LOG_LAST_ERROR("ExceptionEncode failed, len = %d", len);
        len = 0;
    }

    return Send(handle, rpc_head, m_rpc_exception_buff, len);
}

void Rpc::OnRequestProcComplete(const std::string& name,
    int32_t result, int32_t time_cost_ms) {
    if (m_rpc_event_handler) {
        m_rpc_event_handler->OnRequestProcComplete(name, result, time_cost_ms);
    }
}

void Rpc::OnResponseProcComplete(const std::string& name,
    int32_t result, int32_t time_cost_ms) {
    if (m_rpc_event_handler) {
        m_rpc_event_handler->OnResponseProcComplete(name, result, time_cost_ms);
    }
}

} // namespace pebble

