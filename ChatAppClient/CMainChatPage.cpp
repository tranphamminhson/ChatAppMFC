// CMainChatPage.cpp : implementation file

#include "pch.h"
#include "ChatAppClient.h"
#include "afxdialogex.h"
#include "CMainChatPage.h"
#include <sstream>

// CMainChatPage dialog

IMPLEMENT_DYNAMIC(CMainChatPage, CDialogEx)

CMainChatPage::CMainChatPage(SOCKET hSocket, CString sUsername, CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_DIALOG1, pParent),
	m_hSocket(hSocket),         // Lưu socket
	m_sMyUsername(sUsername),  // Username của mình
	m_hRecvThread(NULL),      // Khởi tạo Handle luồng
	m_hStopEvent(NULL),       // Khởi tạo Event
	m_sCurrentChatUser(_T("")) // Chưa chọn ai
{

}

CMainChatPage::~CMainChatPage()
{

}

void CMainChatPage::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_LIST1, m_listUsers);
	DDX_Control(pDX, IDC_RICHEDIT21, m_chatHistory);
	DDX_Control(pDX, IDC_EDIT1, m_editMessage);
	DDX_Control(pDX, IDC_BUTTON1, m_btnSend);
}


BEGIN_MESSAGE_MAP(CMainChatPage, CDialogEx)
	// --- Map các sự kiện UI ---
	ON_WM_DESTROY() // Bắt sự kiện đóng cửa sổ
	ON_BN_CLICKED(IDC_BUTTON1, &CMainChatPage::OnBnClickedButton1) // Nút Send
	ON_BN_CLICKED(IDC_BUTTON_SAVE, &CMainChatPage::OnBnClickedButtonSave) // Nút Save Chat History
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST1, &CMainChatPage::OnLvnItemchangedList1) // Click List User

	// --- Map các Message tùy chỉnh từ luồng ---
	ON_MESSAGE(WM_USER_UPDATE_USERS, &CMainChatPage::OnUpdateUsers)
	ON_MESSAGE(WM_USER_UPDATE_STATUS, &CMainChatPage::OnUserStatusUpdate)
	ON_MESSAGE(WM_USER_RECV_MSG, &CMainChatPage::OnReceiveMessage)
	ON_MESSAGE(WM_USER_HISTORY_START, &CMainChatPage::OnHistoryStart)
	ON_MESSAGE(WM_USER_HISTORY_MSG, &CMainChatPage::OnHistoryMessage)
	ON_MESSAGE(WM_USER_HISTORY_END, &CMainChatPage::OnHistoryEnd)
	ON_MESSAGE(WM_USER_CONNECTION_LOST, &CMainChatPage::OnConnectionLost)
	ON_MESSAGE(WM_USER_DOWN_COMPLETED, &CMainChatPage::OnDownloadCompleted)
END_MESSAGE_MAP()

#ifndef EM_SETTYPOGRAPHYOPTIONS
#define EM_SETTYPOGRAPHYOPTIONS (WM_USER + 202)
#endif
#ifndef TO_ADVANCEDTYPOGRAPHY
#define TO_ADVANCEDTYPOGRAPHY 1
#endif
// CMainChatPage message handlers

BOOL CMainChatPage::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	CString sTitle;
	sTitle.Format(_T("Main Chat - %s"), m_sMyUsername);
	SetWindowText(sTitle);
	// List Users
	m_listUsers.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	m_listUsers.InsertColumn(0, _T("Username"), LVCFMT_LEFT, 180);
	m_listUsers.InsertColumn(1, _T("Status"), LVCFMT_LEFT, 100);

	// Tạo Event Dừng (Stop Event)
	m_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!m_hStopEvent)
	{
		AfxMessageBox(_T("Lỗi nghiêm trọng: Không thể tạo Stop Event!"), MB_ICONERROR);
		EndDialog(IDCANCEL);
		return TRUE;
	}

	// Luồng nhận
	m_hRecvThread = AfxBeginThread(ReceiverThread, this);
	if (!m_hRecvThread)
	{
		AfxMessageBox(_T("Lỗi nghiêm trọng: Không thể tạo Luồng Nhận!"), MB_ICONERROR);
		EndDialog(IDCANCEL);
		return TRUE;
	}

	// GET users
	SendSocketCommand(_T("GET_USERS"));


	// Thiết lập font mặc định cho khung chat
	m_chatHistory.SendMessage(EM_SETTYPOGRAPHYOPTIONS, TO_ADVANCEDTYPOGRAPHY, TO_ADVANCEDTYPOGRAPHY);
	CHARFORMAT2 cf;
	memset(&cf, 0, sizeof(CHARFORMAT2));
	cf.cbSize = sizeof(CHARFORMAT2);
	cf.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET;

	_tcscpy_s(cf.szFaceName, _T("Segoe UI Emoji"));
	cf.yHeight = 200;
	cf.bCharSet = DEFAULT_CHARSET;

	m_chatHistory.SetDefaultCharFormat((CHARFORMAT&)cf);

	return TRUE;  // return TRUE unless you set the focus to a control
}

void CMainChatPage::OnDestroy()
{
	CDialogEx::OnDestroy();

	if (m_hStopEvent)
	{
		SetEvent(m_hStopEvent);
	}

	if (m_hSocket != INVALID_SOCKET)
	{
		shutdown(m_hSocket, SD_BOTH);
		closesocket(m_hSocket);
		m_hSocket = INVALID_SOCKET;
	}

	if (m_hRecvThread)
	{
		WaitForSingleObject(m_hRecvThread->m_hThread, 3000);
		// Không cần CloseHandle(m_hRecvThread) vì AfxBeginThread tự quản lý
	}

	if (m_hStopEvent)
	{
		CloseHandle(m_hStopEvent);
	}
}


// Gửi lệnh qua socket
bool CMainChatPage::SendSocketCommand(const CString& sCommand)
{
	if (m_hSocket == INVALID_SOCKET) return false;

	std::wstring wCmd(sCommand);

	CW2A utf8Cmd(sCommand, CP_UTF8);
	std::string cmd = std::string(utf8Cmd) + "\n";

	int sendResult = send(m_hSocket, cmd.c_str(), (int)cmd.length(), 0);

	if (sendResult == SOCKET_ERROR)
	{
		PostMessage(WM_USER_CONNECTION_LOST);
		return false;
	}
	return true;
}


// Thêm một dòng text vào lịch sử chat
void CMainChatPage::AppendTextToHistory(const CString& sText, COLORREF color)
{
	long nStart, nEnd;

	// Di chuyển con trỏ xuống cuối
	m_chatHistory.SetSel(-1, -1);
	m_chatHistory.GetSel(nStart, nEnd);

	// Thêm text và xuống dòng
	m_chatHistory.ReplaceSel(sText + _T("\r\n"));

	// Chuẩn bị định dạng
	CHARFORMAT2 cf;
	memset(&cf, 0, sizeof(CHARFORMAT2));
	cf.cbSize = sizeof(CHARFORMAT2);
	cf.dwMask = CFM_COLOR | CFM_EFFECTS | CFM_FACE | CFM_CHARSET;
	cf.crTextColor = color;
	cf.dwEffects = 0;
	cf.bCharSet = DEFAULT_CHARSET;
	_tcscpy_s(cf.szFaceName, _T("Segoe UI Emoji"));

	// Chọn lại đoạn text vừa thêm và áp dụng format
	long nNewEnd = nStart + sText.GetLength();
	m_chatHistory.SetSel(nStart, nNewEnd);
	m_chatHistory.SetSelectionCharFormat((CHARFORMAT&)cf);

	// Cuộn xuống cuối
	m_chatHistory.SetSel(-1, -1);
	m_chatHistory.PostMessage(WM_VSCROLL, SB_BOTTOM, 0);
}

// Luồng nhận dữ liệu từ server
UINT __cdecl CMainChatPage::ReceiverThread(LPVOID pParam)
{
	// Lấy con trỏ đến cửa sổ Dialog
	CMainChatPage* pPage = static_cast<CMainChatPage*>(pParam);
	SOCKET hSocket = pPage->m_hSocket;
	HANDLE hStopEvent = pPage->m_hStopEvent;

	char recvBuf[2048];
	int recvResult;

	while (true)
	{
		// Kiểm tra tín hiệu Stop trước mỗi lần nhận
		if (WaitForSingleObject(hStopEvent, 0) == WAIT_OBJECT_0)
		{
			break;
		}

		// Chờ nhận dữ liệu (Blocking)
		recvResult = recv(hSocket, recvBuf, sizeof(recvBuf), 0);

		if (recvResult > 0)
		{
			// Thêm dữ liệu vào bộ đệm (m_sReceiveBuffer)
			pPage->m_sReceiveBuffer.append(recvBuf, recvResult);

			// Xử lý bộ đệm, tìm các lệnh hoàn chỉnh
			size_t pos;
			while ((pos = pPage->m_sReceiveBuffer.find('\n')) != std::string::npos)
			{
				// Lấy ra một lệnh
				std::string cmd = pPage->m_sReceiveBuffer.substr(0, pos);
				// Xóa lệnh đó khỏi bộ đệm
				pPage->m_sReceiveBuffer.erase(0, pos + 1);

				if (!cmd.empty() && cmd.back() == '\r') {
					cmd.pop_back();
				}

				// Gửi lệnh này về UI Thread để xử lý
				pPage->ProcessServerCommand(cmd);
			}
		}
		else
		{
			//eror or connection closed
			if (WaitForSingleObject(hStopEvent, 0) != WAIT_OBJECT_0)
			{
				pPage->PostMessage(WM_USER_CONNECTION_LOST);
			}
			break;
		}
	}

	return 0;
}

// Xử lý lệnh từ server và PostMessage về UI thread
void CMainChatPage::ProcessServerCommand(const std::string& sCommand)
{
	if (sCommand.empty()) return;

	CA2W unicodeCmd(sCommand.c_str(), CP_UTF8);
	CString sCmdLine(unicodeCmd);

	// Tách lệnh và nội dung
	std::stringstream ss(sCommand);
	std::string command;
	ss >> command;
	// Lấy phần còn lại của command
	int nTokenPos = sCmdLine.Find(_T(' '));
	CString sData = (nTokenPos == -1) ? _T("") : sCmdLine.Mid(nTokenPos + 1);

	// Phân loại lệnh và PostMessage tương ứng
	if (command == "USERS_LIST")
	{
		// Hiển thị danh sách user
		PostMessage(WM_USER_UPDATE_USERS, 0, (LPARAM)sData.AllocSysString());
	}
	else if (command == "STATUS_UPDATE")
	{
		// Cập nhật trạng thái user
		PostMessage(WM_USER_UPDATE_STATUS, 0, (LPARAM)sData.AllocSysString());
	}
	else if (command == "RECV")
	{
		// Nhận tin nhắn mới
		PostMessage(WM_USER_RECV_MSG, 0, (LPARAM)sData.AllocSysString());
	}
	else if (command == "HISTORY_START")
	{
		// Bắt đầu tải lịch sử chat
		PostMessage(WM_USER_HISTORY_START);
	}
	else if (command == "MSG_ME" || command == "MSG_THEM")
	{
		// Tin nhắn trong lịch sử chat
		WPARAM type = (command == "MSG_ME") ? 1 : 0;
		PostMessage(WM_USER_HISTORY_MSG, type, (LPARAM)sData.AllocSysString());
	}
	else if (command == "HISTORY_END")
	{
		// Kết thúc tải lịch sử chat
		PostMessage(WM_USER_HISTORY_END);
	}
	else if (command == "DOWN_START")
	{
		// Bắt đầu tải file, xóa bộ đệm cũ
		m_sDownloadBuffer.clear();
	}
	else if (command == "DOWN_LINE")
	{
		// Thêm từng dòng dữ liệu file vào bộ đệm
		size_t firstSpace = sCommand.find(' ');
		if (firstSpace != std::string::npos) {
			std::string lineContent = sCommand.substr(firstSpace + 1);
			m_sDownloadBuffer += lineContent + "\r\n";
		}
	}
	else if (command == "DOWN_END")
	{
		// Báo về UI thread là đã tải xong -> mở dialog lưu file
		PostMessage(WM_USER_DOWN_COMPLETED);
	}
	// TODO: Xử lý các lệnh từ server
}

// Cập nhật trạng thái user
LRESULT CMainChatPage::OnUserStatusUpdate(WPARAM wParam, LPARAM lParam)
{
	CString sData = (LPCTSTR)lParam;

	// Tách Username và Status
	int nPos = sData.Find(_T(' '));
	if (nPos != -1)
	{
		CString sTargetUser = sData.Left(nPos);
		CString sNewStatus = sData.Mid(nPos + 1);

		// Duyệt qua ListCtrl để tìm user cần update
		int nCount = m_listUsers.GetItemCount();
		for (int i = 0; i < nCount; i++)
		{
			CString sUserInList = m_listUsers.GetItemText(i, 0);
			if (sUserInList == sTargetUser)
			{
				m_listUsers.SetItemText(i, 1, sNewStatus); // cập nhật
				break;
			}
		}

	}

	SysFreeString((BSTR)lParam);
	return 0;
}


// Click nút Send
void CMainChatPage::OnBnClickedButton1()
{
	// Kiểm tra đã chọn người nhận chưa
	if (m_sCurrentChatUser.IsEmpty())
	{
		AfxMessageBox(_T("Vui lòng chọn một người dùng từ danh sách để bắt đầu chat."), MB_ICONWARNING);
		return;
	}

	// Lấy nội dung tin nhắn
	CString sMessage;
	m_editMessage.GetWindowText(sMessage);
	sMessage.Trim(); // Xóa khoảng trắng thừa

	if (sMessage.IsEmpty())
	{
		return;
	}

	// Lệnh SEND
	CString sCommand;
	sCommand.Format(_T("SEND %s %s"), m_sCurrentChatUser, sMessage);

	// Gửi lệnh
	if (SendSocketCommand(sCommand))
	{
		AppendTextToHistory(_T("You: ") + sMessage, RGB(255, 0, 0));
		m_editMessage.SetWindowText(_T(""));
	}
	else
	{
		AfxMessageBox(_T("Lỗi gửi tin nhắn. Kết nối có thể đã mất."), MB_ICONERROR);
	}
}

// Xử lý click vào List User
void CMainChatPage::OnLvnItemchangedList1(NMHDR* pNMHDR, LRESULT* pResult)
{
	LPNMLISTVIEW pNMLV = reinterpret_cast<LPNMLISTVIEW>(pNMHDR);
	*pResult = 0;

	if ((pNMLV->uNewState & LVIS_SELECTED) && !(pNMLV->uOldState & LVIS_SELECTED))
	{
		// Lấy vị trí item được chọn
		int nItem = pNMLV->iItem;
		if (nItem == -1) return;

		// Lấy tên user từ item đó
		CString sSelectedUser = m_listUsers.GetItemText(nItem, 0);
		// Cập nhật user đang chat
		m_sCurrentChatUser = sSelectedUser;
		
		// Yêu cầu lịch sử chat với user này
		CString sCommand;
		sCommand.Format(_T("GET_HISTORY %s"), m_sCurrentChatUser);

		SendSocketCommand(sCommand);

	}
}


// Cập nhật danh sách user
LRESULT CMainChatPage::OnUpdateUsers(WPARAM wParam, LPARAM lParam)
{
	CString sData = (LPCTSTR)lParam;
	m_listUsers.DeleteAllItems();

	int nPos = 0;
	CString sToken = sData.Tokenize(_T(" "), nPos);
	int nItem = 0;

	while (!sToken.IsEmpty())
	{
		// sToken có dạng "User:Status"
		int nSplit = sToken.Find(_T(':'));
		if (nSplit != -1)
		{
			CString sUser = sToken.Left(nSplit);
			CString sStatus = sToken.Mid(nSplit + 1);

			// Thêm dòng
			int nIndex = m_listUsers.InsertItem(nItem++, sUser);
			// Thêm cột Status
			m_listUsers.SetItemText(nIndex, 1, sStatus);
		}
		else
		{
			m_listUsers.InsertItem(nItem++, sToken);
		}

		sToken = sData.Tokenize(_T(" "), nPos);
	}

	SysFreeString((BSTR)lParam);
	return 0;
}

// Nhận tin nhắn mới
LRESULT CMainChatPage::OnReceiveMessage(WPARAM wParam, LPARAM lParam)
{
	CString sData = (LPCTSTR)lParam;

	// Tách Tên người gửi và Nội dung
	int nPos = sData.Find(_T(' '));
	if (nPos != -1)
	{
		CString sSender = sData.Left(nPos);
		CString sMessage = sData.Mid(nPos + 1);

		// Chỉ hiển thị tin nhắn nếu đang chat với người đó
		if (sSender == m_sCurrentChatUser)
		{
			CString sDisplay;
			sDisplay.Format(_T("%s: %s"), sSender, sMessage);
			AppendTextToHistory(sDisplay, RGB(0, 0, 255));
		}
		else
		{
			// TODO: Báo hiệu có tin nhắn mới từ người khác
		}
	}

	SysFreeString((BSTR)lParam);
	return 0;
}

// Bắt đầu load lịch sử
LRESULT CMainChatPage::OnHistoryStart(WPARAM wParam, LPARAM lParam)
{
	m_chatHistory.SetWindowText(_T("")); // Xóa sạch lịch sử cũ
	return 0;
}

// Nhận một tin nhắn và hiển thị trong lịch sử
LRESULT CMainChatPage::OnHistoryMessage(WPARAM wParam, LPARAM lParam)
{
	CString sMessage = (LPCTSTR)lParam;
	bool bIsMe = (wParam == 1); // 1 = ME, 0 = THEM

	if (bIsMe)
	{
		AppendTextToHistory(_T("You: ") + sMessage, RGB(255, 0, 0));
	}
	else
	{
		CString sDisplay;
		sDisplay.Format(_T("%s: %s"), m_sCurrentChatUser, sMessage);
		AppendTextToHistory(sDisplay, RGB(0, 0, 255));
	}

	SysFreeString((BSTR)lParam);
	return 0;
}

// Kết thúc load lịch sử
LRESULT CMainChatPage::OnHistoryEnd(WPARAM wParam, LPARAM lParam)
{
	m_chatHistory.PostMessage(WM_VSCROLL, SB_BOTTOM, 0);
	return 0;
}

// Xử lý mất kết nối
LRESULT CMainChatPage::OnConnectionLost(WPARAM wParam, LPARAM lParam)
{
	// Vô hiệu hóa UI
	m_editMessage.EnableWindow(FALSE);
	m_btnSend.EnableWindow(FALSE);
	m_listUsers.EnableWindow(FALSE);

	AfxMessageBox(_T("Đã mất kết nối đến server. Vui lòng khởi động lại ứng dụng."), MB_ICONERROR);

	EndDialog(IDCANCEL);

	return 0;
}


BOOL CMainChatPage::PreTranslateMessage(MSG* pMsg)
{
	// Kiểm tra nếu phím nhấn là Enter (VK_RETURN)
	if (pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_RETURN)
	{
		// Kiểm tra xem focus có đang ở ô nhập tin nhắn không
		if (GetFocus() == &m_editMessage)
		{
			// Gọi hàm xử lý nút Send
			OnBnClickedButton1();
			return TRUE;
		}
	}
	return CDialogEx::PreTranslateMessage(pMsg);
}


void CMainChatPage::OnBnClickedButtonSave()
{
	if (m_sCurrentChatUser.IsEmpty()) {
		AfxMessageBox(_T("Chưa chọn người dùng để lưu tin nhắn!"), MB_ICONWARNING);
		return;
	}

	// Gửi lệnh yêu cầu tải về
	CString sCommand;
	sCommand.Format(_T("REQ_DOWNLOAD %s"), m_sCurrentChatUser);

	if (SendSocketCommand(sCommand)) {
		SetWindowText(_T("Đang tải lịch sử chat..."));
	}
}

LRESULT CMainChatPage::OnDownloadCompleted(WPARAM wParam, LPARAM lParam)
{
	// Trả lại title cũ
	CString sTitle;
	sTitle.Format(_T("Main Chat - %s"), m_sMyUsername);
	SetWindowText(sTitle);

	if (m_sDownloadBuffer.empty()) {
		AfxMessageBox(_T("Không có lịch sử chat để lưu."), MB_ICONINFORMATION);
		return 0;
	}

	// Mở hộp thoại Save
	CFileDialog dlg(FALSE, _T("txt"), _T("ChatHistory.txt"),
		OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
		_T("Text Files (*.txt)|*.txt||"));

	if (dlg.DoModal() == IDOK)
	{
		CString sFilePath = dlg.GetPathName();
		CFile file;

		// Mở file ở chế độ Binary
		if (file.Open(sFilePath, CFile::modeCreate | CFile::modeWrite | CFile::typeBinary))
		{
			//  Ghi BOM cho UTF-8
			unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
			file.Write(bom, 3);

			// Ghi nội dung
			file.Write(m_sDownloadBuffer.c_str(), (UINT)m_sDownloadBuffer.length());

			file.Close();
			AfxMessageBox(_T("Đã lưu tin nhắn thành công!"), MB_ICONINFORMATION);
		}
		else
		{
			AfxMessageBox(_T("Lỗi ghi file!"), MB_ICONERROR);
		}
	}

	// Xóa buffer giải phóng bộ nhớ
	m_sDownloadBuffer.clear();

	return 0;
}