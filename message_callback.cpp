//#include "message_callback.h"
//#include "../Public/y_public_include.h"
//#include "control_module.h"
//
////服务内部使用的异步消息
//void __stdcall ServiceAsyncCallback(const char* topic, const char* contentjs, bool bNotifyUpMessage)
//{
//	//Y_LOGI("%s TOPIC=%s", __Y_FUNC__, topic);
//
//    if (YString::Equals("test", topic))
//    {
//
//    }
//
//
//	if (bNotifyUpMessage)
//	{
//		//TODO
//        //Y_LOGI("RPC to Astraliser: topic=%s, content=%s", topic, contentjs);
//		g_y_communicate_ctrl_module->PushNotify(topic, contentjs);
//	}
//}
//
//
//
//void __stdcall OnRESTfulRsp(Y_TAP_CTX* tapctx, int status_code, const ZMJSON& rspjson, ZMJSON& result, ZMJSON& error)
//{
//
//}