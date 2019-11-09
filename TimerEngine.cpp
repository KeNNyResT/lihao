#include "StdAfx.h"
#include "TimerEngine.h"
#include "log.h"
#include "AttemperEngine.h"

#define TIMER_SPACE								25				            //ʱ���� (windows��Ϊ���뼶��)


//���캯��
CTimerEngine::CTimerEngine(void)
{
	m_bService = false;
	m_QueueService.SetQueueServiceSink(this);
}

//��������
CTimerEngine::~CTimerEngine(void)
{
	//ֹͣ����
	ConcludeService();

	//�����ڴ�
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

//�ӿڲ�ѯ
void * CTimerEngine::QueryInterface(const IID & Guid, DWORD dwQueryVer)
{
	QUERYINTERFACE(ITimerEngine, Guid, dwQueryVer);
	QUERYINTERFACE_IUNKNOWNEX(ITimerEngine, Guid, dwQueryVer);
	return NULL;
}

//���ö�ʱ��
bool CTimerEngine::SetTimer(DWORD dwTimerID, DWORD dwElapse, DWORD dwRepeat, WPARAM dwBindParameter)
{
	//������Դ
	CWHDataLocker lock(m_CriticalSection);

	//Ч�����
	if (dwRepeat == 0) return false;

	CLog::Log(log_debug, "[timer] set timer [%d: %d: %d]", dwTimerID, dwElapse, dwRepeat);

	//���Ҷ�ʱ��
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

	//������ʱ��
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

	//���ò���
	pTimerItem->wTimerID = dwTimerID;
	pTimerItem->wBindParam = dwBindParameter;
	pTimerItem->dwElapse = dwElapse;
	pTimerItem->dwRepeatTimes = dwRepeat;
	pTimerItem->dwStartTickCount = GetTickCount();

	//��ǰ20�����Ƚ���֪ͨ - TIMER_SPACE * 20
	if (pTimerItem->dwRepeatTimes == 1)
		pTimerItem->dwTimeLeave = __max(TIMER_SPACE, pTimerItem->dwElapse - TIMER_SPACE * 20);
	else
		pTimerItem->dwTimeLeave = pTimerItem->dwElapse;

	//���ʱ��
	if (bTimerExist == false)
		m_TimerItemActive.Add(pTimerItem);

	return true;
}

//ɾ����ʱ��
bool CTimerEngine::KillTimer(DWORD dwTimerID)
{
	CLog::Log(log_debug, "[timer] kill timer [%d]", dwTimerID);

	//������Դ
	CWHDataLocker lock(m_CriticalSection);

	//���Ҷ�ʱ��
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

//��ȡ��ʱ��ʣ��ʱ�䣨���룩
DWORD CTimerEngine::GetTimerLeftTickCount(DWORD dwTimerID)
{
	//������Դ
	CWHDataLocker lock(m_CriticalSection);

	//���Ҷ�ʱ��
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

//ɾ����ʱ��
bool CTimerEngine::KillAllTimer()
{
	CLog::Log(log_debug, "[timer] kill all timer");

	//������Դ
	CWHDataLocker lock(m_CriticalSection);

	//ɾ����ʱ��
	m_TimerItemFree.Append(m_TimerItemActive);
	m_TimerItemActive.RemoveAll();

	return true;
}

//��ʼ����
bool CTimerEngine::StartService()
{
	//Ч��״̬
	if (m_bService == true)
	{
		CLog::Log(log_warn, "��ʱ�������ظ�������������������");
		return false;
	}

	//�������з���
	if(! m_QueueService.StartService())
	{
		CLog::Log(log_warn, "��ʱ���ص�δ���ã�����ʧ��");
		return false;
	}

	//�����̷߳���
	if (StartThread() == false)
	{
		CLog::Log(log_error, "��ʱ�������̷߳�������ʧ��");
		return false;
	}

	SetThreadPriority(GetThreadHandle(), REALTIME_PRIORITY_CLASS);


	//���ñ���
	m_bService = true;

	CLog::Log(log_debug, "[Timer] Start Success");

	return true;
}

//ֹͣ����
bool CTimerEngine::ConcludeService()
{
	//���ñ���
	m_bService = false;

	//ֹͣ�߳�
	ConcludeThread(INFINITE);

	//���ñ���
	m_TimerItemFree.Append(m_TimerItemActive);
	m_TimerItemActive.RemoveAll();

	CLog::Log(log_debug, "[Timer] Conculude");

	return true;
}

//�߳����к���
bool CTimerEngine::OnEventThreadRun()
{
	//��������
	DWORD begin = GetTickCount();

	Sleep(TIMER_SPACE); //����
	CWHDataLocker lock(m_CriticalSection);

	DWORD now = GetTickCount();
	DWORD end = now - begin; // TODONOW ���ﲻ����20������????

	//��ѯ��ʱ��
	tagTimerItem * pTimerItem = NULL;
	for (INT_PTR i = 0; i <  m_TimerItemActive.GetCount() ; i++)
	{
		pTimerItem = m_TimerItemActive[i];
		if (pTimerItem == NULL) continue;
		
		//��ʱ������ -- ���ж�ʱ������ǰ TIMER_SPACE���� ����
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
			//���ô���
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

			//����ʱ��
			if (bKillTimer == false)//��ǰ20�����Ƚ���֪ͨ - TIMER_SPACE * 20
			{
				if (pTimerItem->dwRepeatTimes == 1)
					pTimerItem->dwTimeLeave = __max(TIMER_SPACE, pTimerItem->dwElapse - TIMER_SPACE * 20);
				else
					pTimerItem->dwTimeLeave = pTimerItem->dwElapse;
			}

			CLog::Log(log_debug, "[timer] call back timer [%d]", pTimerItem->wTimerID);

			//Ͷ����Ϣ
			PostTimerEvent(pTimerItem->wTimerID, pTimerItem->wBindParam);
		}
	}

	return true;
}

//��ʱ��
bool CTimerEngine::PostTimerEvent(DWORD wTimerID, WPARAM wBindParam)
{
	//��������
	CWHDataLocker lock(m_CriticalSection);

	//Ͷ����Ϣ
	NTY_TimerEvent  pTimerEvent;
	pTimerEvent.dwTimerID = wTimerID;
	pTimerEvent.dwBindParameter = wBindParam;
	m_QueueService.AddToQueue(EVENT_TIMER, &pTimerEvent, sizeof(NTY_TimerEvent));

	return true;
}

//�����¼�
void CTimerEngine::OnQueueServiceSink(WORD wIdentifier, void * pBuffer, WORD wDataSize)
{
	g_CAttemperEngine->OnTimerEngineSink(wIdentifier, pBuffer, wDataSize);
}

//////////////////////////////////////////////////////////////////////////

//����������
extern "C" __declspec(dllexport) void * CreateTimerEngine(const GUID & Guid, DWORD dwInterfaceVer)
{
	//��������
	CTimerEngine * pTimerEngine = NULL;
	try
	{
		pTimerEngine = new CTimerEngine();
		if (pTimerEngine == NULL) throw TEXT("����ʧ��");
		void * pObject = pTimerEngine->QueryInterface(Guid, dwInterfaceVer);
		if (pObject == NULL) throw TEXT("�ӿڲ�ѯʧ��");
		return pObject;
	}
	catch (...) {}

	//�������
	SafeDelete(pTimerEngine);
	return NULL;
}