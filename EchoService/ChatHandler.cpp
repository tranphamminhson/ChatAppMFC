// ChatHandler.cpp

#include "ChatHandler.h"
#include "DebugLog.h"
#include "DbConnect.h"
#include <sstream>
#include <map>
#include <mutex>
#include <vector>

// Khai báo các biến map online từ Service.cpp
extern std::map<std::string, SOCKET> g_onlineUsers;
extern std::mutex g_onlineUsersMutex;

// SendResponse từ Service.cpp
bool SendResponse(SOCKET clientSocket, const std::string& response);


// Lấy UserId từ Username.
int GetUserId(const std::string& username)
{
    if (!g_db) return -1;

    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT UserId FROM Users WHERE Username = ?;";
    int userId = -1;

    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_LOG(L"Lỗi prepare (GetUserId): %S", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        userId = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return userId;
}

// Save Message to DB
sqlite3_int64 SaveMessageToDb(int senderId, int receiverId, const std::string& content)
{
    if (!g_db) return -1;

    sqlite3_stmt* stmt = NULL;
    const char* sql = "INSERT INTO Messages (SenderId, ReceiverId, Content, SentDate) "
        "VALUES (?, ?, ?, datetime('now', 'localtime'));";

    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_LOG(L"Lỗi prepare (SaveMessage): %S", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, senderId);
    sqlite3_bind_int(stmt, 2, receiverId);
    sqlite3_bind_text(stmt, 3, content.c_str(), -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        DEBUG_LOG(L"Lỗi step (SaveMessage): %S", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_int64 lastId = sqlite3_last_insert_rowid(g_db);
    sqlite3_finalize(stmt);

    DEBUG_LOG(L"Đã lưu tin nhắn (ID: %lld) từ %d đến %d", lastId, senderId, receiverId);
    return lastId;
}



// Xử lý lệnh từ client.

void ProcessClientMessage(SOCKET senderSocket, const std::string& senderUsername, const std::string& messageLine)
{
    std::stringstream ss(messageLine);
    std::string command;
    ss >> command;

    if (command == "SEND")
    {
        std::string recipientUsername;
        std::string messageContent;

        ss >> recipientUsername;

        // Lấy phần còn lại của dòng làm nội dung tin nhắn
        std::getline(ss, messageContent);
        // Xóa khoảng trắng thừa ở đầu
        if (!messageContent.empty() && messageContent[0] == ' ') {
            messageContent.erase(0, 1);
        }

        if (recipientUsername.empty() || messageContent.empty()) {
            DEBUG_LOG(L"Lệnh SEND không hợp lệ từ %S", senderUsername.c_str());
            SendResponse(senderSocket, "ERR_INVALID_MSG_FORMAT");
            return;
        }

        // Lấy ID từ DB
        int senderId = GetUserId(senderUsername);
        int recipientId = GetUserId(recipientUsername);

        if (senderId == -1) {
            DEBUG_LOG(L"Không tìm thấy SenderId cho %S", senderUsername.c_str());
            SendResponse(senderSocket, "ERR_FATAL_SENDER");
            return;
        }
        if (recipientId == -1) {
            DEBUG_LOG(L"Không tìm thấy RecipientId cho %S", recipientUsername.c_str());
            SendResponse(senderSocket, "ERR_RECIPIENT_NOT_FOUND");
            return;
        }

        // Lưu tin nhắn vào DB
        sqlite3_int64 messageId = SaveMessageToDb(senderId, recipientId, messageContent);
        if (messageId == -1) {
            SendResponse(senderSocket, "ERR_SAVE_MSG_FAILED");
            return;
        }

        SendResponse(senderSocket, "SEND_OK");

        SOCKET recipientSocket = INVALID_SOCKET;

        // Khóa mutex để truy cập map g_onlineUsers
        {
            std::lock_guard<std::mutex> lock(g_onlineUsersMutex);
            auto it = g_onlineUsers.find(recipientUsername);
            if (it != g_onlineUsers.end()) {
                // found
                recipientSocket = it->second;
            }
        }

        if (recipientSocket != INVALID_SOCKET)
        {
            // Người nhận đang online, gửi tin nhắn cho họ
            std::string forwardMessage = "RECV " + senderUsername + " " + messageContent;

            if (SendResponse(recipientSocket, forwardMessage))
            {
                // Gửi thành công, cập nhật DB
                DEBUG_LOG(L"Đã chuyển tiếp tin nhắn %lld đến %S (Online)", messageId, recipientUsername.c_str());
            }
            else
            {
                // Bị lỗi khi gửi (có thể client vừa ngắt kết nối)
                DEBUG_LOG(L"Lỗi khi chuyển tiếp tin nhắn %lld đến %S (Socket Error)", messageId, recipientUsername.c_str());
            }
        }
        else
        {
            // Người nhận offline, tin nhắn đã được lưu
            DEBUG_LOG(L"Người nhận %S đang offline, tin nhắn %lld sẽ được gửi sau.", recipientUsername.c_str(), messageId);
        }
    }
    else if (command == "GET_USERS")
    {
        HandleGetUsersList(senderSocket, senderUsername);
    }
    else if (command == "GET_HISTORY")
    {
        std::string otherUsername;
        ss >> otherUsername;
        HandleGetHistory(senderSocket, senderUsername, otherUsername);
    }
    else if (command == "REQ_DOWNLOAD")
    {
        std::string otherUsername;
        ss >> otherUsername;
        HandleDownloadRequest(senderSocket, senderUsername, otherUsername);
    }
    // TODO: Thêm các lệnh khác
    else
    {
        DEBUG_LOG(L"Lệnh không xác định '%S' từ %S", command.c_str(), senderUsername.c_str());
        SendResponse(senderSocket, "ERR_UNKNOWN_COMMAND");
    }
}


// GET userlist
void HandleGetUsersList(SOCKET senderSocket, const std::string& currentUsername)
{
    if (!g_db) {
        SendResponse(senderSocket, "ERR_DB_ERROR");
        return;
    }

    sqlite3_stmt* stmt = NULL;
    // Lấy tất cả Username, sắp xếp theo tên
    const char* sql = "SELECT Username FROM Users WHERE Username <> ? ORDER BY Username;";

    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_LOG(L"Lỗi prepare (GetUsersList): %S", sqlite3_errmsg(g_db));
        SendResponse(senderSocket, "ERR_DB_ERROR");
        return;
    }

    // Bind username hiện tại để loại ra
    sqlite3_bind_text(stmt, 1, currentUsername.c_str(), -1, SQLITE_TRANSIENT);

    std::string response = "USERS_LIST";

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const char* username = (const char*)sqlite3_column_text(stmt, 0);
        std::string sUser(username);
        std::string status = "Offline";

        // Kiểm tra user có trong map online không
        {
            std::lock_guard<std::mutex> lock(g_onlineUsersMutex);
            if (g_onlineUsers.find(sUser) != g_onlineUsers.end()) {
                status = "Online";
            }
        }

        response += " ";
        response += sUser + ":" + status; // Gửi dạng User:Status
    }

    sqlite3_finalize(stmt);

    // Gửi danh sách về cho client
    SendResponse(senderSocket, response);
    DEBUG_LOG(L"Đã gửi danh sách user cho Client [%d]", (int)senderSocket);
}



// Lấy lịch sự chat
void HandleGetHistory(SOCKET senderSocket, const std::string& senderUsername, const std::string& otherUsername)
{
    if (otherUsername.empty()) {
        SendResponse(senderSocket, "ERR_INVALID_HISTORY_REQUEST");
        return;
    }
    // lấy ID của 2 user
    int senderId = GetUserId(senderUsername);
    int otherId = GetUserId(otherUsername);

    if (senderId == -1 || otherId == -1) {
        DEBUG_LOG(L"Không tìm thấy ID cho %S hoặc %S", senderUsername.c_str(), otherUsername.c_str());
        SendResponse(senderSocket, "ERR_USER_NOT_FOUND");
        return;
    }

    if (!g_db) {
        SendResponse(senderSocket, "ERR_DB_ERROR");
        return;
    }

    sqlite3_stmt* stmt = NULL;
    // Lấy các tin nhắn giữa 2 người (senderId, otherId)
    // Sắp xếp theo ngày gửi
    const char* sql = "SELECT SenderId, Content, SentDate FROM Messages "
        "WHERE (SenderId = ? AND ReceiverId = ?) OR (SenderId = ? AND ReceiverId = ?) "
        "ORDER BY SentDate ASC;";

    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_LOG(L"Lỗi prepare (GetHistory): %S", sqlite3_errmsg(g_db));
        SendResponse(senderSocket, "ERR_DB_ERROR");
        return;
    }

    // Bind các ID
    sqlite3_bind_int(stmt, 1, senderId);
    sqlite3_bind_int(stmt, 2, otherId);
    sqlite3_bind_int(stmt, 3, otherId);
    sqlite3_bind_int(stmt, 4, senderId);

    // Send message
    SendResponse(senderSocket, "HISTORY_START " + otherUsername);
    DEBUG_LOG(L"Bắt đầu gửi lịch sử chat giữa %S và %S", senderUsername.c_str(), otherUsername.c_str());

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        int msgSenderId = sqlite3_column_int(stmt, 0);
        const char* content = (const char*)sqlite3_column_text(stmt, 1);
        // const char* sentDate = (const char*)sqlite3_column_text(stmt, 2);

        // Xác định tin nhắn này là "của tôi" (MSG_ME) hay "của họ" (MSG_THEM)
        std::string msgType = (msgSenderId == senderId) ? "MSG_ME" : "MSG_THEM";

        // <MSG_TYPE> <Nội dung>

        std::string historyLine = msgType + " " + std::string(content);

        // Gửi từng dòng tin nhắn
        SendResponse(senderSocket, historyLine);
    }

    sqlite3_finalize(stmt);

    // Gửi tin nhắn kết thúc
    SendResponse(senderSocket, "HISTORY_END");
    DEBUG_LOG(L"Hoàn tất gửi lịch sử chat cho Client [%d]", (int)senderSocket);
}


void HandleDownloadRequest(SOCKET senderSocket, const std::string& senderUsername, const std::string& otherUsername)
{
    if (otherUsername.empty()) return;

    int senderId = GetUserId(senderUsername);
    int otherId = GetUserId(otherUsername);

    if (senderId == -1 || otherId == -1) {
        SendResponse(senderSocket, "ERR_USER_NOT_FOUND");
        return;
    }

    if (!g_db) return;

    sqlite3_stmt* stmt = NULL;

    // Query lấy cả SenderId và SentDate
    const char* sql = "SELECT SenderId, Content, SentDate FROM Messages "
        "WHERE (SenderId = ? AND ReceiverId = ?) OR (SenderId = ? AND ReceiverId = ?) "
        "ORDER BY SentDate ASC;";

    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_LOG(L"Lỗi prepare (Download): %S", sqlite3_errmsg(g_db));
        return;
    }

    sqlite3_bind_int(stmt, 1, senderId);
    sqlite3_bind_int(stmt, 2, otherId);
    sqlite3_bind_int(stmt, 3, otherId);
    sqlite3_bind_int(stmt, 4, senderId);

    // Gửi tín hiệu bắt đầu tải
    SendResponse(senderSocket, "DOWN_START");

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        int msgSenderId = sqlite3_column_int(stmt, 0);
        const char* content = (const char*)sqlite3_column_text(stmt, 1);
        const char* sentDate = (const char*)sqlite3_column_text(stmt, 2); // Định dạng DB: YYYY-MM-DD HH:MM:SS

        // Xác định tên người hiển thị
        std::string displayName = (msgSenderId == senderId) ? senderUsername : otherUsername;

        std::string strDate(sentDate);
        //if (strDate.length() > 16) strDate = strDate.substr(0, 16); //bỏ giây

        // Format: [YYYY-MM-DD HH:MM:SS] Name: Content
        std::string fullLine = "[" + strDate + "] " + displayName + ": " + std::string(content);

        // Gửi về Client: DOWN_LINE <Nội dung>
        SendResponse(senderSocket, "DOWN_LINE " + fullLine);
    }

    sqlite3_finalize(stmt);

    // Gửi tín hiệu kết thúc
    SendResponse(senderSocket, "DOWN_END");
    DEBUG_LOG(L"Đã gửi dữ liệu tải xuống cho %S", senderUsername.c_str());
}