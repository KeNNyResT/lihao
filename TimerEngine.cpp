#include "StdAfx.h"
#include "TimerEngine.h"
#include "log.h"
#include "AttemperEngine.h"

#define TIMER_SPACE								25				            //时间间隔 (windows下为毫秒级别)


//构造函数
CTimerEngine::CTimerEngine(void)
{
	m_bService = false;
	m_QueueService.SetQueueServiceSink(this);
}

//析构函数
CTimerEngine::~CTimerEngine(void)
{
	//停止服务
	ConcludeService();

	//清理内存
	tagTimerItem * pTimerItem = NULL;
	for (INT_PTR i = 0; i < m_TimerItemFree.GetCount(); i++)
	{
		pTimerItem = m_TimerItemFree[i];
		SafeDelete(pTimerItem);
	}
	for (INT_PTR i = 0; i < m_TimerItemActive.GetCount(); i++)
	{
		pTimerItem = m_TimerItemActive[i];
		SafeDelete(pTimerItem);
	}
	m_TimerItemFree.RemoveAll();
	m_TimerItemActive.RemoveAll();

	return;
}

//接口查询
void * CTimerEngine::QueryInterface(const IID & Guid, DWORD dwQueryVer)
{
	QUERYINTERFACE(ITimerEngine, Guid, dwQueryVer);
	QUERYINTERFACE_IUNKNOWNEX(ITimerEngine, Guid, dwQueryVer);
	return NULL;
}

//设置定时器
bool CTimerEngine::SetTimer(DWORD dwTimerID, DWORD dwElapse, DWORD dwRepeat, WPARAM dwBindParameter)
{
	//锁定资源
	CWHDataLocker lock(m_CriticalSection);

	//效验参数
	if (dwRepeat == 0) return false;

	CLog::Log(log_debug, "[timer] set timer [%d: %d: %d]", dwTimerID, dwElapse, dwRepeat);

	//查找定时器
	bool bTimerExist = false;
	tagTimerItem * pTimerItem = NULL;
	for (INT_PTR i = 0; i < m_TimerItemActive.GetCount(); i++)
	{
		pTimerItem = m_TimerItemActive[i];
		if (pTimerItem->wTimerID == dwTimerID)
		{
			bTimerExist = true;
			break;
		}
	}

	//创建定时器
	if (bTimerExist == false)
	{
		INT_PTR nFreeCount = m_TimerItemFree.GetCount();
		if (nFreeCount > 0)
		{
			pTimerItem = m_TimerItemFree[nFreeCount-1];
			m_TimerItemFree.RemoveAt(nFreeCount - 1);
		}
		else
		{
			try
			{
				pTimerItem = new tagTimerItem;
				if (pTimerItem == NULL) return false;
			}
			catch (...)
			{
				return false;
			}
		}
	}

	//设置参数
	pTimerItem->wTimerID = dwTimerID;
	pTimerItem->wBindParam = dwBindParameter;
	pTimerItem->dwElapse = dwElapse;
	pTimerItem->dwRepeatTimes = dwRepeat;
	pTimerItem->dwStartTickCount = GetTickCount();

	//提前20个粒度进行通知 - TIMER_SPACE * 20
	if (pTimerItem->dwRepeatTimes == 1)
		pTimerItem->dwTimeLeave = __max(TIMER_SPACE, pTimerItem->dwElapse - TIMER_SPACE * 20);
	else
		pTimerItem->dwTimeLeave = pTimerItem->dwElapse;

	//激活定时器
	if (bTimerExist == false)
		m_TimerItemActive.Add(pTimerItem);

	return true;
}

//删除定时器
bool CTimerEngine::KillTimer(DWORD dwTimerID)
{
	CLog::Log(log_debug, "[timer] kill timer [%d]", dwTimerID);

	//锁定资源
	CWHDataLocker lock(m_CriticalSection);

	//查找定时器
	tagTimerItem * pTimerItem = NULL;
	for (INT_PTR i = 0; i < m_TimerItemActive.GetCount(); i++)
	{
		pTimerItem = m_TimerItemActive[i];
		if (pTimerItem && (pTimerItem->wTimerID == dwTimerID))
		{
			m_TimerItemActive.RemoveAt(i);
			m_TimerItemFree.Add(pTimerItem);
			return true;
		}
	}

	return false;
}

//获取定时器剩余时间（毫秒）
DWORD CTimerEngine::GetTimerLeftTickCount(DWORD dwTimerID)
{
	//锁定资源
	CWHDataLocker lock(m_CriticalSection);

	//查找定时器
	tagTimerItem * pTimerItem = NULL;
	for (INT_PTR i = 0; i < m_TimerItemActive.GetCount(); i++)
	{
		pTimerItem = m_TimerItemActive[i];
		if (pTimerItem && (pTimerItem->wTimerID == dwTimerID))
		{
			return GetTickCount() - pTimerItem->dwStartTickCount;
		}
	}
	return 0;
}

//删除定时器
bool CTimerEngine::KillAllTimer()
{
	CLog::Log(log_debug, "[timer] kill all timer");

	//锁定资源
	CWHDataLocker lock(m_CriticalSection);

	//删除定时器
	m_TimerItemFree.Append(m_TimerItemActive);
	m_TimerItemActive.RemoveAll();

	return true;
}

//开始服务
bool CTimerEngine::StartService()
{
	//效验状态
	if (m_bService == true)
	{
		CLog::Log(log_warn, "定时器引擎重复启动，启动操作忽略");
		return false;
	}

	//开启队列服务
	if(! m_QueueService.StartService())
	{
		CLog::Log(log_warn, "定时器回调未设置，启动失败");
		return false;
	}

	//启动线程服务
	if (StartThread() == false)
	{
		CLog::Log(log_error, "定时器引擎线程服务启动失败");
		return false;
	}

	SetThreadPriority(GetThreadHandle(), REALTIME_PRIORITY_CLASS);


	//设置变量
	m_bService = true;

	CLog::Log(log_debug, "[Timer] Start Success");

	return true;
}

//停止服务
bool CTimerEngine::ConcludeService()
{
	//设置变量
	m_bService = false;

	//停止线程
	ConcludeThread(INFINITE);

	//设置变量
	m_TimerItemFree.Append(m_TimerItemActive);
	m_TimerItemActive.RemoveAll();

	CLog::Log(log_debug, "[Timer] Conculude");

	return true;
}

//线程运行函数
bool CTimerEngine::OnEventThreadRun()
{
	//缓冲锁定
	DWORD begin = GetTickCount();

	Sleep(TIMER_SPACE); //毫秒
	CWHDataLocker lock(m_CriticalSection);

	DWORD now = GetTickCount();
	DWORD end = now - begin; // TODONOW 这里不就是20毫秒吗????

	//查询定时器
	tagTimerItem * pTimerItem = NULL;
	for (INT_PTR i = 0; i <  m_TimerItemActive.GetCount() ; i++)
	{
		pTimerItem = m_TimerItemActive[i];
		if (pTimerItem == NULL) continue;
		
		//定时器处理 -- 所有定时器都提前 TIMER_SPACE毫秒 处理
		bool bKillTimer = false;
		if(pTimerItem->dwTimeLeave > end)
		{
			pTimerItem->dwTimeLeave -= end;
		}
		else
		{
			pTimerItem->dwTimeLeave = 0;
		}

		if (pTimerItem->dwTimeLeave == 0L)
		{
			//设置次数
			if (pTimerItem->dwRepeatTimes != TIMES_INFINITY)
			{
				pTimerItem->dwRepeatTimes--;
				if (pTimerItem->dwRepeatTimes == 0L)
				{
					bKillTimer = true;
					m_TimerItemActive.RemoveAt(i);
					m_TimerItemFree.Add(pTimerItem);
				}
			}

			//设置时间
			if (bKillTimer == false)//提前20个粒度进行通知 - TIMER_SPACE * 20
			{
				if (pTimerItem->dwRepeatTimes == 1)
					pTimerItem->dwTimeLeave = __max(TIMER_SPACE, pTimerItem->dwElapse - TIMER_SPACE * 20);
				else
					pTimerItem->dwTimeLeave = pTimerItem->dwElapse;
			}

			CLog::Log(log_debug, "[timer] call back timer [%d]", pTimerItem->wTimerID);

			//投递消息
			PostTimerEvent(pTimerItem->wTimerID, pTimerItem->wBindParam);
		}
	}

	return true;
}

//定时器
bool CTimerEngine::PostTimerEvent(DWORD wTimerID, WPARAM wBindParam)
{
	//缓冲锁定
	CWHDataLocker lock(m_CriticalSection);

	//投递消息
	NTY_TimerEvent  pTimerEvent;
	pTimerEvent.dwTimerID = wTimerID;
	pTimerEvent.dwBindParameter = wBindParam;
	m_QueueService.AddToQueue(EVENT_TIMER, &pTimerEvent, sizeof(NTY_TimerEvent));

	return true;
}

//队列事件
void CTimerEngine::OnQueueServiceSink(WORD wIdentifier, void * pBuffer, WORD wDataSize)
{
	g_CAttemperEngine->OnTimerEngineSink(wIdentifier, pBuffer, wDataSize);
}

//////////////////////////////////////////////////////////////////////////

//建立对象函数
extern "C" __declspec(dllexport) void * CreateTimerEngine(const GUID & Guid, DWORD dwInterfaceVer)
{
	//建立对象
	CTimerEngine * pTimerEngine = NULL;
	try
	{
		pTimerEngine = new CTimerEngine();
		if (pTimerEngine == NULL) throw TEXT("创建失败");
		void * pObject = pTimerEngine->QueryInterface(Guid, dwInterfaceVer);
		if (pObject == NULL) throw TEXT("接口查询失败");
		return pObject;
	}
	catch (...) {}

	//清理对象
	SafeDelete(pTimerEngine);
	return NULL;
}