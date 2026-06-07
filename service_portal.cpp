#include "service_portal.h"
#include "zm_net_tap.h"
#include "zm_logger.h"
#include "zm_util_thread.h"


void ServicePortal::ResponseResult(ZM_TAP_CTX* tap, const ZMJSON& jsResult)
{
    ZMJSON jsresult;
    jsresult["result"] = jsResult;
    Response(tap, jsresult);
}

void ServicePortal::ResponseError(ZM_TAP_CTX* tap, const ZMJSON& jsError)
{
    ZMJSON jserror;
    jserror["error"] = jsError;
    Response(tap, jserror);
}

void ServicePortal::Response(ZM_TAP_CTX* tap, const ZMJSON& jsResult)
{
    ZmTapDelegate* back_delegate = ZmTapContext::BackChainPop(tap);
    if (back_delegate)
    {
        std::string jstr = jsResult.dump();
        ZmTapContext::SetOnBackData(tap, jstr.size(), jstr.c_str());
        back_delegate->OnTapDelegateBackEvent(tap);
    }
    else
    {
        DEFAULT_LOG_WARN("TAP 回传链为空，无法写入响应，TAP:{}", (void*)tap);
        tap->Drop("back chain empty");
    }
}

/**
 * @brief 异步写入 JRPC 成功响应（可在任意线程中调用）
 *
 * 将响应 JSON 序列化为字符串后通过 ScheduleFn 回投到 libevent 线程，
 * 在线程安全的环境中通过回传链将响应写回客户端。
 *
 * 回投前序列化 JSON 是为了避免跨线程访问 ZMJSON 对象可能的数据竞争
 * （nlohmann::json 非线程安全）。回投后重新解析为 ZMJSON 再走同步路径。
 */
void ServicePortal::ResponseResultAsync(ZM_TAP_CTX* tap, const ZMJSON& jsResult)
{
    if (!m_scheduleFn)
    {
        DEFAULT_LOG_ERROR("ScheduleFn 未设置，无法异步响应，TAP:{}", (void*)tap);
        return;
    }

    ZMJSON jsresult;
    jsresult["result"] = jsResult;
    std::string rspJson = jsresult.dump();

    bool scheduled = m_scheduleFn(
        [this, tap, rspJson](void*) {
            // 校验 TAP 是否仍然存活（可能在异步处理期间被 Drop）
            if (tap->state != ZM_TAP_STATE_INUSE)
            {
                DEFAULT_LOG_WARN("TAP 已失效，丢弃异步响应，TAP:{}, state:{}",
                    (void*)tap, tap->state);
                return;
            }

            ZMJSON js;
            std::string err;
            js = zm_json_parse(rspJson, err);
            if (!err.empty())
            {
                DEFAULT_LOG_ERROR("异步响应 JSON 解析失败: {}，TAP:{}", err, (void*)tap);
                tap->Drop("async response json parse error");
                return;
            }
            Response(tap, js);
        },
        nullptr, nullptr);

    if (!scheduled)
    {
        DEFAULT_LOG_ERROR("ScheduleFn 调度失败，事件循环可能已停止，TAP:{}", (void*)tap);
    }
}

void ServicePortal::ResponseErrorAsync(ZM_TAP_CTX* tap, const ZMJSON& jsError)
{
    if (!m_scheduleFn)
    {
        DEFAULT_LOG_ERROR("ScheduleFn 未设置，无法异步响应，TAP:{}", (void*)tap);
        return;
    }

    ZMJSON jserror;
    jserror["error"] = jsError;
    std::string rspJson = jserror.dump();

    bool scheduled = m_scheduleFn(
        [this, tap, rspJson](void*) {
            if (tap->state != ZM_TAP_STATE_INUSE)
            {
                DEFAULT_LOG_WARN("TAP 已失效，丢弃异步错误响应，TAP:{}, state:{}",
                    (void*)tap, tap->state);
                return;
            }

            ZMJSON js;
            std::string err;
            js = zm_json_parse(rspJson, err);
            if (!err.empty())
            {
                DEFAULT_LOG_ERROR("异步错误响应 JSON 解析失败: {}，TAP:{}", err, (void*)tap);
                tap->Drop("async error response json parse error");
                return;
            }
            Response(tap, js);
        },
        nullptr, nullptr);

    if (!scheduled)
    {
        DEFAULT_LOG_ERROR("ScheduleFn 调度失败，事件循环可能已停止，TAP:{}", (void*)tap);
    }
}

/**
 * @brief JRPC 请求回调（在 libevent 线程中由 TAP 代理链触发）
 *
 * 提供两种处理模式示例：
 * 1. 同步模式（默认）：直接调用 ResponseResult 在当前线程中响应
 *    适用于业务逻辑轻量、无需耗时 I/O 的场景
 * 2. 异步模式（注释示例）：将业务逻辑投递到线程池，处理完成后通过
 *    ResponseResultAsync 回写响应
 *    适用于需要耗时计算、数据库查询、外部 API 调用等场景
 *
 * ZM_TAP_STATE_INUSE
 * ZM_TAP_CTX
 * ZmThreadPool
 */
void ServicePortal::JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData)
{
    //// =========================================================================
    //// 同步响应模式（当前默认）
    //// 适用于无需耗时的简单业务
    //// =========================================================================
    //ZMJSON rsp = { { "isOk", 1 } };
    //ResponseResult(tap, rsp);

    // =========================================================================
    // 异步响应模式示例（取消注释以启用）
    // 适用于需要耗时处理的业务
    // =========================================================================
    
     // 1. 拷贝请求数据（tap->requester_data 在回调返回后可能被覆盖）
     std::string reqCopy(reqData);
    
     // 2. 投递到线程池处理业务
     ZmThreadPool::InvokeLater([this, tap, reqCopy](void*) {
         // ===== Worker 线程 =====
         // 在此执行耗时业务逻辑
    
         ZMJSON result;
         std::string err;
         ZMJSON reqJson = zm_json_parse(reqCopy, err);
    
         if (!err.empty())
         {
             ZMJSON errRsp;
             errRsp["code"] = -32700;
             errRsp["message"] = "Parse error: " + err;
             ResponseErrorAsync(tap, errRsp);
             return;
         }
    
         std::string method = zm_json_get_str(reqJson, "method");
         ZMJSON params = reqJson["params"];
    
         // 根据 method 分发业务逻辑
         if (method == "ping")
         {
             result["pong"] = true;
         }
         else if (method == "getStatus")
         {
             result["status"] = "running";
             result["uptime"] = 12345;
         }
         else
         {
             ZMJSON errRsp;
             errRsp["code"] = -32601;
             errRsp["message"] = "Method not found: " + method;
             ResponseErrorAsync(tap, errRsp);
             return;
         }
    
         // 3. 通过异步接口写回响应（内部自动回投到 libevent 线程）
         ResponseResultAsync(tap, result);
    
     }, nullptr, 0);
    
     // 注意：异步模式下此函数 return 后不再调用任何响应方法，
     // 响应由 Worker 线程回调中的 ResponseResultAsync/ResponseErrorAsync 完成
}