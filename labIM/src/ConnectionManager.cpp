﻿#include "ConnectionManager.h"
#include "IOContextManager.h"
#include "NetStructureManager.h"
#include "MessageManager.h"
#include "DBop.h"

const StringType connManagefamilyStr("ConnManage");
const StringType sendSingleActionStr("SendSingle");
const StringType sendGroupActionStr("SendGroup");
const StringType sendBroadcastActionStr("SendBroadcast");
const StringType sendRandomActionStr("SendRandom");

const int maxRouteCount = 1;

ConnectionManager::ConnectionManager()
{
	validConn[ConnType::CONN_PARENT] = ConnMap();
	validConn[ConnType::CONN_BROTHER] = ConnMap();
	validConn[ConnType::CONN_CHILD] = ConnMap();
	validConn[ConnType::CONN_TEMP] = ConnMap();

	registerFamilyHandler(connManagefamilyStr, std::bind(&ConnectionManager::actionParse, this, _1, _2));

	registerActionHandler(sendSingleActionStr, std::bind(&ConnectionManager::handleMsgSingle, this, _1, _2));
	registerActionHandler(sendGroupActionStr, std::bind(&ConnectionManager::handleMsgGroup, this, _1, _2));
	registerActionHandler(sendBroadcastActionStr, std::bind(&ConnectionManager::handleMsgBroadcast, this, _1, _2));
}

ConnectionManager::~ConnectionManager()
{
}

ConnectionManager* ConnectionManager::getInstance()
{
    static ConnectionManager instance;
    return &instance;
}

void ConnectionManager::registerObj(StringType id, ConnImplType type, ConnPtr conn)
{
	//多线程调用可能导致ID生成错误
	static int temConnID = 0;

	if (id == INVALID_ID) {
		id = std::to_string(++temConnID).c_str();
	}

	conn->setID(id);
	conn->start();
	if (validConn.find(type) != validConn.end()) {
		validConn[type][id] = conn;
	}

	//这最好记录日志
}

void ConnectionManager::unregisterObj(const StringType& id)
{
	for (auto& conns : validConn) {
		if (conns.second.erase(id))
			return;
	}
}

const QHash<QString, QStringList>& ConnectionManager::getUserGroupMap()
{
	static bool isInit = false;

	if (!isInit) {
		userGroupMap = getUsersJoinGroups();
		isInit = true;
	}

	return userGroupMap;
}

ConnPtr ConnectionManager::findConn(const StringType & id)
{
	for (auto& conns : validConn) {
		auto it = conns.second.find(id);
		if (it != conns.second.end()) {
			return it->second;
		}
	}

	return ConnPtr();
}

ConnPtr ConnectionManager::connnectHost(ConnImplType type, const StringType& id, JsonObjType& addr, ServicePtr servicePtr, ConnectHandler&& handler)
{
	HostDescription hd;
    hd.uuid = addr["uid"].toString().toStdString();
    hd.ip = addr["uip"].toString().toStdString();
    hd.mac = addr["umac"].toString().toStdString();

	setHostArp(hd.ip, hd.mac);
    tcp::socket sock(IOContextManager::getInstance()->getIOLoop());
	auto conn = std::make_shared<Connection>(std::move(sock), hd, ConnectionManager::getInstance(), servicePtr);
	conn->connect(type, id, std::move(handler));
	return conn;
}

void ConnectionManager::sendtoConn(const StringType& id, JsonObjType msg)
{
	auto conn = findConn(id);
	if (conn.get() != nullptr) {
		conn->send(msg);
	}
}

QHash<QString, QStringList> ConnectionManager::getUsersJoinGroups()
{
	QHash<QString, QStringList> result;
	auto dbop = DBOP::getInstance();
	for (auto& conns : validConn) {
		for (auto& conn : conns.second) {
			result[conn.first.c_str()] = dbop->listJoinGroup(conn.first.c_str());
		}
	}

	QString localUuid = NetStructureManager::getInstance()->getLocalUuid().c_str();
	result[localUuid] = dbop->listJoinGroup(localUuid);

	return result;
}

void ConnectionManager::sendSingleMsg(JsonObjType& msg, bool isRepackage)
{
	qDebug() << "single msg! isSend: " << isRepackage << " package: "<< msg;

	auto data = isRepackage ? msg["data"].toObject() : msg["data"].toObject()["data"].toObject();
	auto dest = data["dest"].toString().toStdString();
	if (dest == NetStructureManager::getInstance()->getLocalUuid()) {
		familyParse(isRepackage ? msg : msg["data"].toObject(), nullptr);
		return;
	}

	JsonObjType sendMsg;
	if (isRepackage) {
		sendMsg["family"] = connManagefamilyStr.c_str();
		sendMsg["action"] = sendSingleActionStr.c_str();
		sendMsg["data"] = msg;
	}

	auto role = NetStructureManager::getInstance()->getLocalRole();
	switch (role)
	{
	case ROLE_MASTER:
		if (isRepackage){
			auto result = validConn[ConnType::CONN_CHILD].end();
			result = validConn[ConnType::CONN_CHILD].find(dest);
			if (result != validConn[ConnType::CONN_CHILD].end()) {
				result->second->send(sendMsg);
			}
			else {
				if (!validConn[ConnType::CONN_CHILD].empty())
					validConn[ConnType::CONN_CHILD].begin()->second->send(sendMsg);
			}
		}
		break;
	case ROLE_ROUTER:
		{
			auto result = validConn[ConnType::CONN_CHILD].end();
			result = validConn[ConnType::CONN_CHILD].find(dest);
			if (result != validConn[ConnType::CONN_CHILD].end()) {
				result->second->send(isRepackage ? sendMsg : msg);
			}
			else {
				if (!(isRepackage ? sendMsg.contains("routeCount") : msg.contains("routeCount"))) {
					(isRepackage ? sendMsg["routeCount"] : msg["routeCount"]) = 1;
					auto result = validConn[ConnType::CONN_PARENT].end();
					result = validConn[ConnType::CONN_PARENT].find(dest);
					if (result != validConn[ConnType::CONN_PARENT].end()) {
						result->second->send(isRepackage ? sendMsg : msg);
						return;
					}
				}

				int routeCount = isRepackage ? sendMsg["routeCount"].toInt() : msg["routeCount"].toInt();
				(isRepackage ? sendMsg["routeCount"] : msg["routeCount"]) = routeCount + 1;
				if (routeCount >= maxRouteCount) return;

				if (!validConn[ConnType::CONN_BROTHER].empty())
					validConn[ConnType::CONN_BROTHER].begin()->second->send(isRepackage ? sendMsg : msg);
			}
		}
		break;
	case ROLE_MEMBER:
		if (isRepackage) {
			if (!validConn[ConnType::CONN_PARENT].empty())
				validConn[ConnType::CONN_PARENT].begin()->second->send(sendMsg);
		}
		break;
	default:
		break;
	}
}

void ConnectionManager::sendGroupMsg(JsonObjType& msg, bool isRepackage)
{
	qDebug() << "group msg! isSend: " << isRepackage << " package: " << msg;

	getUserGroupMap();

	JsonObjType sendMsg;
	if (isRepackage) {
		sendMsg["family"] = connManagefamilyStr.c_str();
		sendMsg["action"] = sendGroupActionStr.c_str();
		sendMsg["data"] = msg;
	}

	JsonObjType msgData = isRepackage ? msg["data"].toObject() : msg["data"].toObject()["data"].toObject();
	QString groupId = msgData["dest"].toString();
	QString localUuid = NetStructureManager::getInstance()->getLocalUuid().c_str();
	if (userGroupMap[localUuid].contains(groupId))
		familyParse(isRepackage ? msg : msg["data"].toObject(), nullptr);

	auto role = NetStructureManager::getInstance()->getLocalRole();
	switch (role)
	{
	case ROLE_MASTER:
		if (isRepackage) {
			if (!validConn[ConnType::CONN_CHILD].empty())
				validConn[ConnType::CONN_CHILD].begin()->second->send(sendMsg);
		}
		break;
	case ROLE_ROUTER:
		{
			if (!(isRepackage ? sendMsg.contains("routeCount") : msg.contains("routeCount"))) {
				(isRepackage ? sendMsg["routeCount"] : msg["routeCount"]) = 1;
				for (auto& parent : validConn[ConnType::CONN_PARENT]) {
					if (userGroupMap[parent.first.c_str()].contains(groupId))
						parent.second->send(isRepackage ? sendMsg : msg);
				}
			}

			for (auto& child : validConn[ConnType::CONN_CHILD]) {
				if (userGroupMap[child.first.c_str()].contains(groupId))
					child.second->send(isRepackage ? sendMsg : msg);
			}

			int routeCount = isRepackage ? sendMsg["routeCount"].toInt() : msg["routeCount"].toInt();
			(isRepackage ? sendMsg["routeCount"] : msg["routeCount"]) = routeCount + 1;
			if (routeCount >= maxRouteCount) return;

			if (!validConn[ConnType::CONN_BROTHER].empty())
				validConn[ConnType::CONN_BROTHER].begin()->second->send(isRepackage ? sendMsg : msg);
		}
		break;
	case ROLE_MEMBER:
		if (isRepackage)
			if (!validConn[ConnType::CONN_PARENT].empty())
				validConn[ConnType::CONN_PARENT].begin()->second->send(sendMsg);
		break;
	default:
		break;
	}
}

void ConnectionManager::sendBroadcastMsg(JsonObjType& msg, bool isRepackage)
{
	qDebug() << "broadcast msg! isSend: " << isRepackage << " package: " << msg;

	JsonObjType sendMsg;
	if (isRepackage) {
		sendMsg["family"] = connManagefamilyStr.c_str();
		sendMsg["action"] = sendBroadcastActionStr.c_str();
		sendMsg["data"] = msg;
	}

	familyParse(isRepackage ? msg : msg["data"].toObject(), nullptr);

	auto role = NetStructureManager::getInstance()->getLocalRole();
	switch (role)
	{
	case ROLE_MASTER:
		if (isRepackage) {
			if (!validConn[ConnType::CONN_CHILD].empty())
				validConn[ConnType::CONN_CHILD].begin()->second->send(sendMsg);
		}
		break;
	case ROLE_ROUTER:
		{
			if (!(isRepackage ? sendMsg.contains("routeCount") : msg.contains("routeCount"))) {
				(isRepackage ? sendMsg["routeCount"] : msg["routeCount"]) = 1;
				for (auto& parent : validConn[ConnType::CONN_PARENT]) {
					parent.second->send(isRepackage ? sendMsg : msg);
				}
			}

			for (auto& child : validConn[ConnType::CONN_CHILD])
				child.second->send(isRepackage ? sendMsg : msg);

			int routeCount = isRepackage ? sendMsg["routeCount"].toInt() : msg["routeCount"].toInt();
			(isRepackage ? sendMsg["routeCount"] : msg["routeCount"]) = routeCount + 1;
			if (routeCount >= maxRouteCount) return;

			if (!validConn[ConnType::CONN_BROTHER].empty())
				validConn[ConnType::CONN_BROTHER].begin()->second->send(isRepackage ? sendMsg : msg);
		}
		break;
	case ROLE_MEMBER:
		if (isRepackage)
			if (!validConn[ConnType::CONN_PARENT].empty())
				validConn[ConnType::CONN_PARENT].begin()->second->send(sendMsg);
		break;
	default:
		break;
	}
}

QString ConnectionManager::getRandomServiceDest()
{
	getUserGroupMap();

	QString destNode;
	auto role = NetStructureManager::getInstance()->getLocalRole();
	switch (role)
	{
	case ROLE_MASTER:
		if (!validConn[ConnType::CONN_CHILD].empty())
			destNode = validConn[ConnType::CONN_CHILD].begin()->first.c_str();
		break;
	case ROLE_ROUTER:
		destNode = NetStructureManager::getInstance()->getLocalUuid().c_str();
		break;
	case ROLE_MEMBER:
		if (!validConn[ConnType::CONN_PARENT].empty())
			destNode = validConn[ConnType::CONN_PARENT].begin()->first.c_str();
		break;
	default:
		break;
	}
	return destNode;
}

void ConnectionManager::sendRandomMsg(JsonObjType& msg, bool isRepackage)
{
	qDebug() << "random msg! isSend: " << isRepackage << " package: " << msg;

	getUserGroupMap();

	auto role = NetStructureManager::getInstance()->getLocalRole();
	switch (role)
	{
	case ROLE_MASTER:
		if (!validConn[ConnType::CONN_CHILD].empty())
			validConn[ConnType::CONN_CHILD].begin()->second->send(msg);
		break;
	case ROLE_ROUTER:
		if (!validConn[ConnType::CONN_BROTHER].empty())
			validConn[ConnType::CONN_BROTHER].begin()->second->send(msg);
		break;
	case ROLE_MEMBER:
		if (!validConn[ConnType::CONN_PARENT].empty())
			validConn[ConnType::CONN_PARENT].begin()->second->send(msg);
		break;
	default:
		break;
	}
}

void ConnectionManager::handleMsgSingle(JsonObjType & msg, ConnPtr conn)
{
	sendSingleMsg(msg, false);
}

void ConnectionManager::handleMsgBroadcast(JsonObjType & msg, ConnPtr conn)
{
	sendBroadcastMsg(msg, false);
}

void ConnectionManager::handleMsgGroup(JsonObjType & msg, ConnPtr conn)
{
	sendGroupMsg(msg, false);
}

void ConnectionManager::sendActionMsg(TransferMode mode, const StringType & family, const StringType & action, JsonObjType& datas)
{
	JsonObjType msg;
	msg["family"] = family.c_str();
	msg["action"] = action.c_str();
	msg["data"] = datas;

	switch (mode) {
	case Single: sendSingleMsg(msg); break;
	case Group: sendGroupMsg(msg); break;
	case Broadcast: sendBroadcastMsg(msg); break;
	case Random: sendRandomMsg(msg); break;
	default:break;
	}
}

void ConnectionManager::uploadPicMsgToCommonSpace(const QString & groupId, QVariantHash & data, bool isRoute)
{
	getUserGroupMap();

	QStringList destNodes;
	auto role = NetStructureManager::getInstance()->getLocalRole();
	switch (role)
	{
	case ROLE_MASTER:
		if (!isRoute && !validConn[ConnType::CONN_CHILD].empty())
			destNodes.append(validConn[ConnType::CONN_CHILD].begin()->first.c_str());
		break;
	case ROLE_ROUTER: 
		{
			if (!data.contains("routeCount")) {
				data["routeCount"] = 1;
				for (auto& parent : validConn[ConnType::CONN_PARENT]) {
					if (userGroupMap[parent.first.c_str()].contains(groupId))
						destNodes.append(parent.first.c_str());
				}
			}

			for (auto& child : validConn[ConnType::CONN_CHILD]) {
				if (userGroupMap[child.first.c_str()].contains(groupId))
					destNodes.append(child.first.c_str());
			}

			int routeCount = data["routeCount"].toInt();
			data["routeCount"] = routeCount + 1;
			if (routeCount < maxRouteCount && !validConn[ConnType::CONN_BROTHER].empty()) {
				destNodes.append(validConn[ConnType::CONN_BROTHER].begin()->first.c_str());
			}
		}
		break;
	case ROLE_MEMBER:
		if (!isRoute && !validConn[ConnType::CONN_PARENT].empty())
			destNodes.append(validConn[ConnType::CONN_PARENT].begin()->first.c_str());
		break;
	default:
		break;
	}

	for (auto& node : destNodes) {
		auto addr = JsonObjType::fromVariantHash(DBOP::getInstance()->getUser(node));
		auto servicePtr = std::make_shared<PicTransferService>(data["picRealName"].toString(), JsonDocType::fromVariant(QVariant(data)).object());
		ConnectionManager::getInstance()->connnectHost(ConnType::CONN_TEMP, INVALID_ID, addr, servicePtr, [](const boost::system::error_code& err) {
			if (err != 0) {
				qDebug() << "send picture group msg connnection connect failed!";
				return;
			}

			qDebug() << "send picture group msg connnection connect success!";
		});
	}
}

void ConnectionManager::uploadFileToGroupSpace(JsonObjType& sharedFileInfo, bool isRoute)
{
	getUserGroupMap();

	QString destNode;
	auto role = NetStructureManager::getInstance()->getLocalRole();
	switch (role)
	{
	case ROLE_MASTER:
		if (!isRoute && !validConn[ConnType::CONN_CHILD].empty())
			destNode = validConn[ConnType::CONN_CHILD].begin()->first.c_str();
		break;
	case ROLE_ROUTER:
	{
		if (!isRoute) {
			destNode = NetStructureManager::getInstance()->getLocalUuid().c_str();
		}
		else {
			if (!sharedFileInfo.contains("routeCount")) {
				sharedFileInfo["routeCount"] = 1;
			}

			int routeCount = sharedFileInfo["routeCount"].toInt();
			sharedFileInfo["routeCount"] = routeCount + 1;
			if (routeCount < maxRouteCount && !validConn[ConnType::CONN_BROTHER].empty()) {
				destNode = validConn[ConnType::CONN_BROTHER].begin()->first.c_str();
			}
		}
	}
	break;
	case ROLE_MEMBER:
		if (!isRoute && !validConn[ConnType::CONN_PARENT].empty())
			destNode = validConn[ConnType::CONN_PARENT].begin()->first.c_str();
		break;
	default:
		break;
	}

	if (destNode.isEmpty()) return;

	ServicePtr servicePtr;
	if (isRoute) {
		servicePtr = std::make_shared<GroupFileUploadService>(sharedFileInfo, true);
	}
	else {
		servicePtr = std::make_shared<GroupFileUploadService>(sharedFileInfo["fileName"].toString(), sharedFileInfo["fileGroup"].toString());
	}

	auto addr = JsonObjType::fromVariantHash(DBOP::getInstance()->getUser(destNode));
	ConnectionManager::getInstance()->connnectHost(ConnType::CONN_TEMP, INVALID_ID, addr, servicePtr, [](const boost::system::error_code& err) {
		if (err != 0) {
			qDebug() << "upload group file connnection connect failed!";
			return;
		}

		qDebug() << "upload group file connnection connect success!";
	});
}


Connection::Connection(tcp::socket s, const HostDescription& dest, ConnectionManager* cm, ServicePtr servicePtr)
	:sock(std::move(s)), dest(dest), parent(cm), id(INVALID_ID), servicePtr(servicePtr)
{	
}

Connection::Connection(Connection && c)
	: sock(std::move(c.sock)), dest(c.dest), id(c.id), parent(c.parent), servicePtr(c.servicePtr)
{
}

Connection::~Connection()
{
	stop();
}

void Connection::start()
{
	servicePtr->setConn(shared_from_this());
	servicePtr->start();
}

void Connection::connect(ConnImplType type, const StringType& id, ConnectHandler&& handler)
{
	auto self(shared_from_this());
	tcp::endpoint endpoint(make_address_v4(dest.ip), HostDescription::tcpPort);
	//tcp::endpoint endpoint(make_address_v4(getLocalIp()), HostDescription::tcpPort);

	sock.async_connect(endpoint, [this, self, handler, type, id](const boost::system::error_code& err) {
		qDebug() << "connect state: " << err;
        parent->registerObj(StringType(id), type, self);
		handler(err);

		if (err != 0) {
            parent->unregisterObj(id);
			qDebug() << "connect failed";
			return;
		}
	});
}

void Connection::dataHandle()
{
	servicePtr->dataHandle();
}

void Connection::send(JsonObjType rawData)
{
	servicePtr->sendData(rawData);
}

void Connection::execute()
{
	servicePtr->execute();
}

void Connection::restore()
{
	servicePtr->restore();
}

void Connection::pause()
{
	servicePtr->pause();
}

void Connection::stop()
{
	servicePtr->stop();
	sock.close();
    parent->unregisterObj(id);
}

int Connection::getProgress()
{
	return servicePtr->getProgress();
}
