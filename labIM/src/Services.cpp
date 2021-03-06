﻿#include "Services.h"
#include "ConnectionManager.h"
#include "IOContextManager.h"
#include "TaskManager.h"
#include "SessionManager.h"
#include "DataModel.h"
#include "NetStructureManager.h"
#include "SharedFileManager.h"

#include "QtCore\qfile.h"
#include "QtCore\qfileinfo.h"
#include "QtCore\qurl.h"
#include "QtCore\qthread.h"

const QString netStructureServiceStr("NetStructureService");
const QString picTransferServiceStr("PicTransferService");
const QString fileDownloadServiceStr("FileDownloadService");
const QString groupFileUploadServiceStr("GroupFileUploadService");
const QString fileSendServiceStr("FileSendService");
const QString taskPauseStr("TaskPause");
const QString taskStopStr("TaskStop");
const QString taskRestartStr("TaskRestart");

ServicePtr Service::getServicePtr(const QString & name, JsonObjType & params)
{
    if (name == netStructureServiceStr) {
        return std::make_shared<NetStructureService>();
    }
    else if (name == picTransferServiceStr) {
        return std::make_shared<PicTransferService>(params);
    }
	else if (name == fileDownloadServiceStr) {
		return std::make_shared<FileDownloadService>(params);
	}
	else if (name == groupFileUploadServiceStr) {
		return std::make_shared<GroupFileUploadService>(params);
	}
	else if (name == groupFileUploadServiceStr) {
		return std::make_shared<GroupFileUploadService>(params);
	}
	else if (name == fileSendServiceStr) {
		return std::make_shared<FileSendService>(params);
	}

    return ServicePtr();
}

Service::Service()
    : readBuff(BUF_SIZE, '\0')
{
}

Service::~Service()
{
}

void Service::start()
{
    dataHandle();
}

void Service::dataHandle()
{
    conn->sock.async_receive(boost::asio::buffer(readBuff.data(), readBuff.size()), [this](const boost::system::error_code& ec, std::size_t readBytes) {
        if (ec != 0) {
            qDebug() << "tcp connect error: " << ec;
            conn->stop();
            return;
        }

        auto rawMsg = readBuff.length() == readBytes ? readBuff : readBuff.left(readBytes);
        char* dataPtr = rawMsg.data();
        short msgLen = 0;
        memcpy(&msgLen, dataPtr, 2);
        auto serviceInfor = JsonDocType::fromJson(QByteArray(dataPtr + 2, msgLen)).object();
        auto newServicePtr = getServicePtr(serviceInfor["serviceName"].toString(), serviceInfor["serviceParam"].toObject());
		auto oldService = conn->getService(); //extend the object life time
        conn->setService(newServicePtr);

        int remainBytes = readBytes - msgLen - 2;
        if (remainBytes > 0) {
            newServicePtr->setRemain(readBuff.right(remainBytes));
        }
    });
}

void Service::sendData(JsonObjType rawData)
{
    JsonDocType doc(rawData);
    auto sendData = std::make_shared<SendBufferType>(doc.toJson(JSON_FORMAT).data());

    ////此处应该记录日志
    //if (msg->length() + 2 > BUF_SIZE) return;

    auto msgLen = (short)sendData->length();
    char mlenBytes[2]{ '\0' };
    memcpy(mlenBytes, &msgLen, 2);
    sendData->insert(0, mlenBytes, 2);

    conn->sock.async_send(boost::asio::buffer(sendData->data(), sendData->size()), [this, sendData](const boost::system::error_code& ec, std::size_t writeBytes) {
        qDebug() << "TCP SEND  len: " << writeBytes << " data: " << sendData->left(writeBytes);
    });
}

void Service::execute()
{
}

void Service::pause()
{
}

void Service::restore()
{
}

void Service::stop()
{
}

int Service::getProgress()
{
	return 0;
}

void Service::msgHandleLoop(size_t readBytes, RecvBufferType & readBuff, RecvBufferType & readRemain, ConnPtr conn, std::function<void(JsonObjType&)>&& handler)
{
	auto rawMsg = readRemain + (readBuff.length() == readBytes ? readBuff : readBuff.left(readBytes));
	char* dataPtr = rawMsg.data();
	short handleDataLen = 0, msgLen = 0, allDataLen = rawMsg.length();

	qDebug() << "TCP RECV  len: " << readRemain.length() + readBytes << " data: " << rawMsg.left(readRemain.length() + readBytes);

	do
	{
		if (allDataLen - handleDataLen < 2) break;

		memcpy(&msgLen, dataPtr, 2);

		if (allDataLen - handleDataLen < msgLen + 2) break;

		auto msg = JsonDocType::fromJson(QByteArray(dataPtr + 2, msgLen)).object();
		handler(msg);

		dataPtr += msgLen + 2;
		handleDataLen += msgLen + 2;

	} while (allDataLen != handleDataLen);

	if (allDataLen == handleDataLen || allDataLen - handleDataLen > BUF_SIZE) {
		readRemain.clear();
	}
	else {
		readRemain.setRawData(dataPtr, allDataLen - handleDataLen);
	}
}

NetStructureService::NetStructureService()
    : order(INVALID_ORDER)
{
}

NetStructureService::~NetStructureService()
{
}

void NetStructureService::start()
{
    JsonObjType serviceInfor;
    serviceInfor["serviceName"] = netStructureServiceStr;
    Service::sendData(serviceInfor);
    dataHandle();
}

void NetStructureService::dataHandle()
{
    conn->sock.async_receive(boost::asio::buffer(readBuff.data(), readBuff.size()), [this](const boost::system::error_code& ec, std::size_t readBytes) {
        if (ec != 0) {
            qDebug() << "tcp connect error: " << ec;
            conn->stop();
            return;
        }

		msgHandleLoop(readBytes, readBuff, readRemain, conn, [this](JsonObjType& msg) {
			conn->getParent()->familyParse(msg, conn);
		});

        dataHandle();
    });
}

//Picture Transfer Service
PicTransferService::PicTransferService(const QString& fileName, JsonObjType& taskParam)
    : writeBuff(1024*512, '\0'), isInit(false), fileName(fileName), isSender(true), taskParam(taskParam)
{
}

PicTransferService::PicTransferService(JsonObjType& taskParam)
    : isInit(false), fileSize(0), recvFileLen(0), isSender(false), taskParam(taskParam)
{
    readBuff.resize(1024*512);
}

PicTransferService::~PicTransferService()
{
}

void PicTransferService::start()
{
	if (isSender)
	{
		JsonObjType serviceInfor;
		serviceInfor["serviceName"] = picTransferServiceStr;
		serviceInfor["serviceParam"] = taskParam;
		Service::sendData(serviceInfor);
		execute();
	}
	else { dataHandle(); }
}

void PicTransferService::dataHandle()
{
    conn->sock.async_receive(boost::asio::buffer(readBuff.data(), readBuff.size()), [this](const boost::system::error_code& ec, std::size_t readBytes) {
        if (ec != 0) {
            qDebug() << "PicTransferService tcp connect error: " << ec;
			if (picFile.isOpen()) picFile.close();
            conn->stop();
            return;
        }

        auto rawMsg = readBuff.length() == readBytes ? readBuff : readBuff.left(readBytes);
        if (!readRemain.isEmpty()) {
            rawMsg.push_front(readRemain);
            readRemain.clear();
        }

        if (!isInit) {
            fileName = tmpDir.c_str() + taskParam["picStoreName"].toString();
            fileSize = taskParam["picSize"].toInt();
			picFile.setFileName(fileName);
			if (!picFile.open(QFile::WriteOnly)) {
                qDebug() << "write file open failed! filenme:" << fileName;
				conn->stop();
				return;
			}
            isInit = true;
        }

		//qDebug() << "recv file recv executing! filename: " << fileName << " recvBytes: " << readBytes;

        int writeBytes = picFile.write(rawMsg);
		if (writeBytes == -1) {
			qDebug() << "recv picture write failed! filename: " << fileName << " errorCode: " << ec;
			picFile.close();
			conn->stop();
			return;
		}

		//qDebug() << "recv file write executing! filename: " << fileName << " writeBytes: " << writeBytes;
		recvFileLen += writeBytes;
		if (recvFileLen >= fileSize) {
			qDebug() << "recv picture recv finished! filename: " << fileName;
			picFile.close();
			conn->stop();

			QUrl fileUrl = QUrl::fromLocalFile(tmpDir.c_str() + taskParam["picStoreName"].toString());
			MessageInfo msgInfo(taskParam["msgId"].toString(), taskParam["msgSource"].toString(), taskParam["msgDest"].toString(), taskParam["msgType"].toInt(),
				fileUrl.toString(), taskParam["msgDate"].toString(), taskParam["msgMode"].toInt());
			SessionManager::getInstance()->createMessage(msgInfo, false);
			
			if (msgInfo.mmode == (int)SessionType::GroupSession) {
				taskParam["picRealName"] = tmpDir.c_str() + taskParam["picStoreName"].toString();
				ConnectionManager::getInstance()->uploadPicMsgToCommonSpace(msgInfo.mduuid, taskParam.toVariantHash(), true);
			}
			return;
		}

        dataHandle();
    });
}

void PicTransferService::execute()
{
    if (!isInit) {
		picFile.setFileName(fileName);
        if (!picFile.open(QFile::ReadOnly) ) {
            qDebug() << "send file open failed! filenme:" << fileName;
			conn->stop();
			return;
        }

        isInit = true;
    }
    
	int readBytes = picFile.read(writeBuff.data(), writeBuff.size());
	if (readBytes < 0) {
		qDebug() << "send file read failed! filename: " << fileName << " errorCode: " << picFile.errorString();
		picFile.close();
		conn->stop();
		return;
	}
	else if (readBytes == 0) {
		conn->sock.async_send(boost::asio::buffer(writeBuff.data(), readBytes), [this](const boost::system::error_code& ec, std::size_t writeBytes) {
			qDebug() << "send file read finished! filename: " << fileName;
			picFile.close();
		});
		return;
	}
	
	//qDebug() << "send file read executing! filename: " << fileName << " readBytes: " << readBytes;
	conn->sock.async_send(boost::asio::buffer(writeBuff.data(), readBytes), [this](const boost::system::error_code& ec, std::size_t writeBytes) {
		if (ec != 0) {
			qDebug() << "send file send failed! filename: " << fileName << " errorCode: " << ec;
			picFile.close();
			conn->stop();
			return;
		}

		//qDebug() << "send file send executing! filename: " << fileName << " sendBytes: " << writeBytes;
		execute();
	});
}


FileDownloadService::FileDownloadService(const QString & fileName, JsonObjType & taskData)
	:isInit(false), filePath(fileName), fileSize(0), handleFileLen(0), isProvider(false), taskData(taskData)
{
    readBuff.resize(1024*512);
}

FileDownloadService::FileDownloadService(JsonObjType & taskData)
    : isInit(false), isProvider(true), fileSize(0), handleFileLen(0), writeBuff(1024*512, '\0'), taskData(taskData), isExe(true)
{
}

FileDownloadService::~FileDownloadService()
{
}

void FileDownloadService::start()
{
	qDebug() << taskData;
	if (!isProvider) {
		taskId = taskData["taskId"].toString();
		JsonObjType serviceInfor;
		serviceInfor["serviceName"] = fileDownloadServiceStr;
		serviceInfor["serviceParam"] = taskData;
		Service::sendData(serviceInfor);
		dataHandle();
	}
	else {
		TaskInfo task(taskData["rsource"].toString(), taskData["rdest"].toString(), 
            TaskType::FileTransferTask, TransferMode::Single, JsonDocType(taskData).toJson(JsonDocType::Compact));
        taskId = task.tid;
		if (TaskManager::getInstance()->createTask(task, conn) == 0) {
			execute();
			taskControlMsgHandle();
		}
		else {
			conn->stop();
		}
	}
}

void FileDownloadService::dataHandle()
{
	if (!isExe) return;

	conn->sock.async_receive(boost::asio::buffer(readBuff.data(), readBuff.size()), [this](const boost::system::error_code& ec, std::size_t readBytes) {
		if (ec != 0) {
			qDebug() << "FileDownloadService recv data error: " << ec;
			if (file.isOpen()) file.close();
			conn->stop();
			TaskManager::getInstance()->errorTask(taskId);
			return;
		}

		auto rawMsg = readBuff.length() == readBytes ? readBuff : readBuff.left(readBytes);
		if (!readRemain.isEmpty()) {
			rawMsg.push_front(readRemain);
			readRemain.clear();
		}

		if (!isInit) {
			fileSize = taskData["fileSize"].toInt();
			file.setFileName(filePath);
			if (!file.open(QFile::WriteOnly)) {
				qDebug() << "download write file open failed! filenme:" << file;
				conn->stop();
				TaskManager::getInstance()->errorTask(taskId);
				return;
			}
			isInit = true;
		}

		//qDebug() << "download recv file recv executing! filename: " << filePath << " recvBytes: " << readBytes;

		int writeBytes = file.write(rawMsg);
		if (writeBytes == -1) {
			qDebug() << "download recv picture write failed! filename: " << filePath << " errorCode: " << ec;
			if (file.isOpen()) file.close();
			conn->stop();
			TaskManager::getInstance()->errorTask(taskId);
			return;
		}

		//qDebug() << "download recv file write executing! filename: " << filePath << " writeBytes: " << writeBytes;
		handleFileLen += writeBytes;
		if (handleFileLen >= fileSize) {
			qDebug() << "recv picture recv finished! filename: " << filePath;
			file.close();
			TaskManager::getInstance()->finishTask(taskId);
			conn->stop();
			return;
		}

		dataHandle();
	});
}

void FileDownloadService::execute()
{
	if (!isExe) return;

	if (!isInit) {
		fileSize = taskData["fileSize"].toInt();
		filePath = taskData["fileSourePath"].toString();
		file.setFileName(filePath);
		if (!file.open(QFile::ReadOnly)) {
			qDebug() << "download send file open failed! filenme:" << filePath;
			conn->stop();
			TaskManager::getInstance()->errorTask(taskId);
			return;
		}
		isInit = true;
	}

	int readBytes = file.read(writeBuff.data(), writeBuff.size());
	handleFileLen += readBytes;
	if (readBytes < 0) {
		qDebug() << "download send file read failed! filename: " << filePath << " errorCode: " << file.errorString();
		if (file.isOpen()) file.close();
		conn->stop();
		TaskManager::getInstance()->errorTask(taskId);
		return;
	}
	else if (readBytes == 0) {
		conn->sock.async_send(boost::asio::buffer(writeBuff.data(), readBytes), [this](const boost::system::error_code& ec, std::size_t writeBytes) {
			qDebug() << "download send file read finished! filename: " << filePath;
			file.close();
			TaskManager::getInstance()->finishTask(taskId);
		});
		return;
	}

	//qDebug() << "download send file read executing! filename: " << filePath << " readBytes: " << readBytes;
	conn->sock.async_send(boost::asio::buffer(writeBuff.data(), readBytes), [this](const boost::system::error_code& ec, std::size_t writeBytes) {
		if (ec != 0) {
			qDebug() << "download send file send failed! filename: " << filePath << " errorCode: " << ec;
			file.close();
			conn->stop();
			TaskManager::getInstance()->errorTask(taskId);
			return;
		}

		//qDebug() << "download send file send executing! filename: " << filePath << " sendBytes: " << writeBytes;
		execute();
	});
}

void FileDownloadService::pause()
{
	if (isProvider) {
		isExe = false;
	}
	else {
		JsonObjType taskAction;
		taskAction["serviceName"] = taskPauseStr;
		Service::sendData(taskAction);
	}
}

void FileDownloadService::restore()
{
	if (isProvider) {
		isExe = true;
		execute();
	}
	else {
		JsonObjType taskAction;
		taskAction["serviceName"] = taskRestartStr;
		Service::sendData(taskAction);
	}
}

int FileDownloadService::getProgress()
{
	return int(float(handleFileLen) / fileSize * 100);
}

void FileDownloadService::taskControlMsgHandle()
{
	conn->sock.async_receive(boost::asio::buffer(readBuff.data(), readBuff.size()), [this](const boost::system::error_code& ec, std::size_t readBytes){
		if (ec != 0) {
			qDebug() << "FileDownloadService control msg recv error: " << ec;
			return;
		}

		msgHandleLoop(readBytes, readBuff, readRemain, conn, [this](JsonObjType& msg) {
			auto action = msg["serviceName"].toString();
			if (action == taskPauseStr) {
				TaskManager::getInstance()->pauseTask(taskId);
			}
			else if (action == taskRestartStr) {
				TaskManager::getInstance()->restoreTask(taskId);
			}
		});

		taskControlMsgHandle();
	});
}


GroupFileUploadService::GroupFileUploadService(const QString & filePath, const QString& groupId)
	: writeBuff(1024 * 512, '\0'), isInit(false), isSender(true), isExe(true), isRoute(false), fileSize(0), handleFileLen(0), filePath(filePath), groupId(groupId)
{
}

GroupFileUploadService::GroupFileUploadService(JsonObjType & groupFileData, bool)
	: writeBuff(1024 * 512, '\0'), isInit(false), isSender(true), isExe(true), isRoute(true), fileSize(0), handleFileLen(0), groupFileData(groupFileData)
{
}

GroupFileUploadService::GroupFileUploadService(JsonObjType & groupFileData)
	: isInit(false), isSender(false), isRoute(true), fileSize(0), handleFileLen(0), groupFileData(groupFileData)
{
	readBuff.resize(1024 * 512);
}

GroupFileUploadService::~GroupFileUploadService()
{
}

void GroupFileUploadService::start()
{
	if (isSender)
	{
		if (isRoute){
			filePath = groupDir.c_str() + groupFileData["fileName"].toString();
			fileSize = groupFileData["fileSize"].toInt();
		}
		else {
			QFileInfo fileInfo(filePath);
			fileSize = fileInfo.size();
			groupFileData["fileName"] = fileInfo.fileName();
			groupFileData["fileSize"] = fileSize;
			groupFileData["fileGroup"] = groupId;
			groupFileData["fileOwner"] = NetStructureManager::getInstance()->getLocalUuid().c_str();

			TaskInfo task(groupId, TaskType::FileTransferTask, TransferMode::Group, JsonDocType(groupFileData).toJson(JsonDocType::Compact));
			TaskManager::getInstance()->createTask(task, conn);
			taskId = task.tid;
		}

		JsonObjType serviceInfor;
		serviceInfor["serviceName"] = groupFileUploadServiceStr;
		serviceInfor["serviceParam"] = groupFileData;
		Service::sendData(serviceInfor);
		execute();
	}
	else {
		filePath = groupDir.c_str() + groupFileData["fileName"].toString();
		fileSize = groupFileData["fileSize"].toInt();
		dataHandle(); 
	}
}

void GroupFileUploadService::dataHandle()
{
	conn->sock.async_receive(boost::asio::buffer(readBuff.data(), readBuff.size()), [this](const boost::system::error_code& ec, std::size_t readBytes) {
		if (ec != 0) {
			qDebug() << "GroupFileUploadService tcp connect error: " << ec;
			if (file.isOpen()) file.close();
			conn->stop();
			return;
		}

		auto rawMsg = readBuff.length() == readBytes ? readBuff : readBuff.left(readBytes);
		if (!readRemain.isEmpty()) {
			rawMsg.push_front(readRemain);
			readRemain.clear();
		}

		if (!isInit) {
			file.setFileName(filePath);
			if (!file.open(QFile::WriteOnly)) {
				qDebug() << "write group file open failed! filenme:" << filePath;
				conn->stop();
				return;
			}
			isInit = true;
		}

		int writeBytes = file.write(rawMsg);
		if (writeBytes == -1) {
			qDebug() << "recv group file write failed! filePath: " << filePath << " errorCode: " << ec;
			file.close();
			conn->stop();
			return;
		}

		handleFileLen += writeBytes;
		if (handleFileLen >= fileSize) {
			qDebug() << "recv group file recv finished! filePath: " << filePath;
			file.close();
			conn->stop();

			QString sharedFilePath = groupDir.c_str() + groupFileData["fileName"].toString();
			SharedFileInfo sharedFile(sharedFilePath, groupFileData["fileOwner"].toString(), groupFileData["fileGroup"].toString());
			SharedFileManager::getInstance()->addSharedFile(sharedFile);

			ConnectionManager::getInstance()->uploadFileToGroupSpace(groupFileData, true);
			return;
		}

		dataHandle();
	});
}

void GroupFileUploadService::execute()
{
	if (!isExe) return;

	if (!isInit) {
		file.setFileName(filePath);
		if (!file.open(QFile::ReadOnly)) {
			qDebug() << "send group file open failed! filePath:" << filePath;
			conn->stop();
			if (!isRoute) TaskManager::getInstance()->errorTask(taskId);
			return;
		}

		isInit = true;
	}

	int readBytes = file.read(writeBuff.data(), writeBuff.size());
	handleFileLen += readBytes;
	if (readBytes < 0) {
		qDebug() << "send group file read failed! filePath: " << filePath << " errorCode: " << file.errorString();
		file.close();
		conn->stop();
		if (!isRoute) TaskManager::getInstance()->errorTask(taskId);
		return;
	}
	else if (readBytes == 0) {
		conn->sock.async_send(boost::asio::buffer(writeBuff.data(), readBytes), [this](const boost::system::error_code& ec, std::size_t writeBytes) {
			qDebug() << "send group file read finished! filePath: " << filePath;
			file.close();
			if (!isRoute) TaskManager::getInstance()->finishTask(taskId);
		});
		return;
	}

	conn->sock.async_send(boost::asio::buffer(writeBuff.data(), readBytes), [this](const boost::system::error_code& ec, std::size_t writeBytes) {
		if (ec != 0) {
			qDebug() << "send group file send failed! filename: " << filePath << " errorCode: " << ec;
			file.close();
			conn->stop();
			if (!isRoute) TaskManager::getInstance()->errorTask(taskId);
			return;
		}

		execute();
	});
}

void GroupFileUploadService::pause()
{
	if (isSender) {
		isExe = false;
	}
}

void GroupFileUploadService::restore()
{
	if (isSender) {
		isExe = true;
		execute();
	}
}

int GroupFileUploadService::getProgress()
{
	return int(float(handleFileLen) / fileSize * 100);
}


FileSendService::FileSendService(const QString & fileName, const QString & storePath)
	: isSender(true), isInit(false), fileName(fileName), storePath(storePath), writeBuff(1024 * 512, '\0'), handleFileLen(0)
{

}

FileSendService::FileSendService(JsonObjType & serviceParam)
	: isSender(false), isInit(false), handleFileLen(0)
{
	fileName = serviceParam["fileName"].toString();
	fileSize = serviceParam["fileSize"].toInt();
	readBuff.resize(1024 * 512);
}

FileSendService::~FileSendService()
{
}

void FileSendService::start()
{
	if (isSender)
	{
		JsonObjType serviceInfor;
		serviceInfor["serviceName"] = fileSendServiceStr;

		JsonObjType serviceParam;
		QFileInfo fileInfo(fileName);
		serviceParam["fileSize"] = fileInfo.size();
		serviceParam["fileName"] = storePath + "/" + fileInfo.fileName();
		serviceInfor["serviceParam"] = serviceParam;
		Service::sendData(serviceInfor);
		execute();
	}
	else { dataHandle(); }
}

void FileSendService::dataHandle()
{
	if (!isInit) {
		file.setFileName(fileName);
		if (!file.open(QFile::WriteOnly)) {
			qDebug() << "write file open failed! filenme:" << fileName;
			conn->stop();
			return;
		}

		if (!readRemain.isEmpty()) {
			int writeBytes = file.write(readRemain);
			readRemain.clear();
			if (writeBytes == -1) {
				qDebug() << "recv file write failed! filename: " << fileName;
				file.close();
				conn->stop();
				return;
			}

			handleFileLen += writeBytes;
			if (handleFileLen >= fileSize) {
				qDebug() << "recv file recv finished! filename: " << fileName;
				file.close();
				conn->stop();
				return;
			}
		}

		isInit = true;
	}

	conn->sock.async_receive(boost::asio::buffer(readBuff.data(), readBuff.size()), [this](const boost::system::error_code& ec, std::size_t readBytes) {
		if (ec != 0) {
			qDebug() << "FileSendService tcp connect error: " << ec;
			if (file.isOpen()) file.close();
			conn->stop();
			return;
		}

		auto rawMsg = readBuff.length() == readBytes ? readBuff : readBuff.left(readBytes);

		int writeBytes = file.write(rawMsg);
		if (writeBytes == -1) {
			qDebug() << "recv file write failed! filename: " << fileName << " errorCode: " << ec;
			file.close();
			conn->stop();
			return;
		}

		handleFileLen += writeBytes;
		if (handleFileLen >= fileSize) {
			qDebug() << "recv file recv finished! filename: " << fileName;
			file.close();
			conn->stop();
			return;
		}

		dataHandle();
	});
}

void FileSendService::execute()
{
	if (!isInit) {
		file.setFileName(fileName);
		if (!file.open(QFile::ReadOnly)) {
			qDebug() << "send file open failed! filenme:" << fileName;
			conn->stop();
			return;
		}

		isInit = true;
	}

	int readBytes = file.read(writeBuff.data(), writeBuff.size());
	if (readBytes < 0) {
		qDebug() << "send file read failed! filename: " << fileName << " errorCode: " << file.errorString();
		file.close();
		conn->stop();
		return;
	}
	else if (readBytes == 0) {
		conn->sock.async_send(boost::asio::buffer(writeBuff.data(), readBytes), [this](const boost::system::error_code& ec, std::size_t writeBytes) {
			qDebug() << "send file read finished! filename: " << fileName;
			file.close();
		});
		return;
	}

	conn->sock.async_send(boost::asio::buffer(writeBuff.data(), readBytes), [this](const boost::system::error_code& ec, std::size_t writeBytes) {
		if (ec != 0) {
			qDebug() << "send file send failed! filename: " << fileName << " errorCode: " << ec;
			file.close();
			conn->stop();
			return;
		}

		execute();
	});
}