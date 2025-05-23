/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file tcp.h Basic functions to receive and send TCP packets.
 */

#ifndef NETWORK_CORE_TCP_H
#define NETWORK_CORE_TCP_H

#include "address.h"
#include "packet.h"

#include <atomic>
#include <chrono>
#include <thread>

/** The states of sending the packets. */
enum SendPacketsState : uint8_t {
	SPS_CLOSED,      ///< The connection got closed.
	SPS_NONE_SENT,   ///< The buffer is still full, so no (parts of) packets could be sent.
	SPS_PARTLY_SENT, ///< The packets are partly sent; there are more packets to be sent in the queue.
	SPS_ALL_SENT,    ///< All packets in the queue are sent.
};

/** Base socket handler for all TCP sockets */
class NetworkTCPSocketHandler : public NetworkSocketHandler {
private:
	std::deque<std::unique_ptr<Packet>> packet_queue{}; ///< Packets that are awaiting delivery. Cannot be std::queue as that does not have a clear() function.
	std::unique_ptr<Packet> packet_recv = nullptr; ///< Partially received packet

	void EmptyPacketQueue();
public:
	SOCKET sock = INVALID_SOCKET; ///< The socket currently connected to
	bool writable = false; ///< Can we write to this socket?

	/**
	 * Whether this socket is currently bound to a socket.
	 * @return true when the socket is bound, false otherwise
	 */
	bool IsConnected() const { return this->sock != INVALID_SOCKET; }

	virtual NetworkRecvStatus CloseConnection(bool error = true);
	void CloseSocket();

	virtual void SendPacket(std::unique_ptr<Packet> &&packet);
	SendPacketsState SendPackets(bool closing_down = false);

	virtual std::unique_ptr<Packet> ReceivePacket();

	bool CanSendReceive();

	/**
	 * Whether there is something pending in the send queue.
	 * @return true when something is pending in the send queue.
	 */
	bool HasSendQueue() { return !this->packet_queue.empty(); }

	/**
	 * Construct a socket handler for a TCP connection.
	 * @param s The just opened TCP connection.
	 */
	NetworkTCPSocketHandler(SOCKET s = INVALID_SOCKET) : sock(s) {}
	~NetworkTCPSocketHandler();
};

/**
 * "Helper" class for creating TCP connections in a non-blocking manner
 */
class TCPConnecter {
private:
	/**
	 * The current status of the connecter.
	 *
	 * We track the status like this to ensure everything is executed from the
	 * game-thread, and not at another random time where we might not have the
	 * lock on the game-state.
	 */
	enum class Status : uint8_t {
		Init,       ///< TCPConnecter is created but resolving hasn't started.
		Resolving,  ///< The hostname is being resolved (threaded).
		Failure,    ///< Resolving failed.
		Connecting, ///< We are currently connecting.
		Connected,  ///< The connection is established.
	};

	std::thread resolve_thread;                         ///< Thread used during resolving.
	std::atomic<Status> status = Status::Init;          ///< The current status of the connecter.
	std::atomic<bool> killed = false;                   ///< Whether this connecter is marked as killed.

	addrinfo *ai = nullptr;                             ///< getaddrinfo() allocated linked-list of resolved addresses.
	std::vector<addrinfo *> addresses;                  ///< Addresses we can connect to.
	std::map<SOCKET, NetworkAddress> sock_to_address;   ///< Mapping of a socket to the real address it is connecting to. USed for DEBUG statements.
	size_t current_address = 0;                         ///< Current index in addresses we are trying.

	std::vector<SOCKET> sockets;                        ///< Pending connect() attempts.
	std::chrono::steady_clock::time_point last_attempt; ///< Time we last tried to connect.

	std::string connection_string;                      ///< Current address we are connecting to (before resolving).
	NetworkAddress bind_address;                        ///< Address we're binding to, if any.
	int family = AF_UNSPEC;                             ///< Family we are using to connect with.

	static std::vector<std::shared_ptr<TCPConnecter>> connecters; ///< List of connections that are currently being created.

	void Resolve();
	void OnResolved(addrinfo *ai);
	bool TryNextAddress();
	void Connect(addrinfo *address);
	virtual bool CheckActivity();

	/* We do not want any other derived classes from this class being able to
	 * access these private members, but it is okay for TCPServerConnecter. */
	friend class TCPServerConnecter;

	static void ResolveThunk(TCPConnecter *connecter);

public:
	TCPConnecter() {};
	TCPConnecter(std::string_view connection_string, uint16_t default_port, const NetworkAddress &bind_address = {}, int family = AF_UNSPEC);
	virtual ~TCPConnecter();

	/**
	 * Callback when the connection succeeded.
	 * @param s the socket that we opened
	 */
	virtual void OnConnect([[maybe_unused]] SOCKET s) {}

	/**
	 * Callback for when the connection attempt failed.
	 */
	virtual void OnFailure() {}

	void Kill();

	static void CheckCallbacks();
	static void KillAll();

	/**
	 * Create the connecter, and initiate connecting by putting it in the collection of TCP connections to make.
	 * @tparam T The type of connecter to create.
	 * @param args The arguments to the constructor of T.
	 * @return Shared pointer to the connecter.
	 */
	template <class T, typename... Args>
	static std::shared_ptr<TCPConnecter> Create(Args&& ... args)
	{
		return TCPConnecter::connecters.emplace_back(std::make_shared<T>(std::forward<Args>(args)...));
	}
};

class TCPServerConnecter : public TCPConnecter {
private:
	SOCKET socket = INVALID_SOCKET; ///< The socket when a connection is established.

	bool CheckActivity() override;

public:
	ServerAddress server_address; ///< Address we are connecting to.

	TCPServerConnecter(std::string_view connection_string, uint16_t default_port);

	void SetConnected(SOCKET sock);
	void SetFailure();
};

#endif /* NETWORK_CORE_TCP_H */
