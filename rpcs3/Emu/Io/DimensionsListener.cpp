#include "stdafx.h"

// winsock2.h must come before any header that could pull in <windows.h>
// (Dimensions.h -> usb_device.h does), or the legacy winsock.h it includes
// clashes with winsock2.h ('sockaddr' redefinition).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "DimensionsListener.h"
#include "Dimensions.h"
#include "Emu/Io/interception.h"
#include "util/logs.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

LOG_CHANNEL(dim_listener_log, "DIMLISTEN");

namespace
{
#ifdef _WIN32
	using socket_t = SOCKET;
	constexpr socket_t invalid_sock = INVALID_SOCKET;
	void close_sock(socket_t s) { ::closesocket(s); }
#else
	using socket_t = int;
	constexpr socket_t invalid_sock = -1;
	void close_sock(socket_t s) { ::close(s); }
#endif

	std::thread g_listener_thread;
	std::atomic<bool> g_listener_running{false};
	std::atomic<socket_t> g_listen_sock{invalid_sock};
	constexpr auto move_pickup_delay = std::chrono::milliseconds(500);

#ifdef _WIN32
	std::thread g_picker_thread;

	// Companion-app input handoff (same cross-repo contract as the Cemu fork):
	// LegoToypad keeps this manual-reset event signaled while its picker
	// overlay is visible. While signaled, the game's pads are intercepted via
	// RPCS3's own dialog mechanism so button presses used to navigate the
	// picker never reach the game.
	constexpr wchar_t picker_event_name[] = L"Local\\CemuToypadPickerInputActive";

	void picker_watch_run()
	{
		HANDLE event_handle = nullptr;
		u64 last_open_ms = 0;
		bool active_prev = false;
		bool we_intercepted = false;

		while (g_listener_running)
		{
			// Re-open the handle periodically: if the companion app restarts,
			// it creates a fresh event object under the same name and a stale
			// handle would keep watching the dead one.
			const u64 now_ms = GetTickCount64();
			if (!event_handle || now_ms - last_open_ms >= 1000)
			{
				if (event_handle)
					CloseHandle(event_handle);
				event_handle = OpenEventW(SYNCHRONIZE, FALSE, picker_event_name);
				last_open_ms = now_ms;
			}

			const bool active = event_handle && WaitForSingleObject(event_handle, 0) == WAIT_OBJECT_0;
			if (active != active_prev)
			{
				if (active)
				{
					// Leave interception alone if something else (an RPCS3
					// dialog) already holds it - and don't release it later.
					if (!input::g_pads_intercepted)
					{
						input::SetIntercepted(true, false, false);
						we_intercepted = true;
						dim_listener_log.notice("Picker overlay opened: pads intercepted");
					}
				}
				else if (we_intercepted)
				{
					input::SetIntercepted(false, false, false);
					we_intercepted = false;
					dim_listener_log.notice("Picker overlay closed: pads released");
				}
				active_prev = active;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		if (we_intercepted)
			input::SetIntercepted(false, false, false);
		if (event_handle)
			CloseHandle(event_handle);
	}
#endif

	bool recv_all(socket_t s, u8* data, usz len)
	{
		while (len != 0)
		{
			const auto received = ::recv(s, reinterpret_cast<char*>(data), static_cast<int>(len), 0);
			if (received <= 0)
				return false;
			data += received;
			len -= static_cast<usz>(received);
		}
		return true;
	}

	bool send_all(socket_t s, const u8* data, usz len)
	{
		while (len != 0)
		{
			const auto sent = ::send(s, reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
			if (sent <= 0)
				return false;
			data += sent;
			len -= static_cast<usz>(sent);
		}
		return true;
	}

	u16 listener_port()
	{
		if (const char* env = std::getenv("RPCS3_TOYPAD_PORT"))
		{
			const int port = std::atoi(env);
			if (port >= 1 && port <= 65535)
				return static_cast<u16>(port);
		}
		return 9191;
	}

	void handle_client(socket_t client)
	{
		u8 header[5];
		if (!recv_all(client, header, sizeof(header)))
			return;

		const u8 cmd = header[0];
		const u8 pad = header[1];
		const u8 index = header[2];

		// GET_LED (0x04) carries no pad/index (they're 0), so skip the slot
		// validation for it - it would otherwise be rejected below.
		if (cmd != 0x04 && (pad < 1 || pad > 3 || index >= 7))
		{
			dim_listener_log.error("Rejected message: cmd=0x%02x pad=%d index=%d", cmd, pad, index);
			return;
		}

		switch (cmd)
		{
		case 0x01: // LOAD
		{
			std::array<u8, 0x2D * 0x04> tag{};
			if (!recv_all(client, tag.data(), tag.size()))
				return;

			u8 len_buf[2];
			if (!recv_all(client, len_buf, sizeof(len_buf)))
				return;
			const u16 path_len = static_cast<u16>(len_buf[0] | (len_buf[1] << 8));

			fs::file file;
			if (path_len != 0)
			{
				std::vector<u8> path_buf(path_len);
				if (!recv_all(client, path_buf.data(), path_len))
					return;
				const std::string path(reinterpret_cast<const char*>(path_buf.data()), path_len);
				// Optional persistence: game writes go back to this .bin, same
				// as loading it through the Dimensions Manager dialog.
				file.open(path, fs::read + fs::write + fs::lock);
			}

			// The listener contract (LegoToypad expects Cemu-fork behavior) is
			// that LOAD silently overwrites an occupied slot.
			g_dimensionstoypad.remove_figure(pad, index, true, true);
			g_dimensionstoypad.load_figure(tag, std::move(file), pad, index, true);
			dim_listener_log.notice("LOAD -> pad=%d index=%d (path len %d)", pad, index, path_len);
			break;
		}
		case 0x02: // REMOVE
		{
			g_dimensionstoypad.remove_figure(pad, index, true, true);
			dim_listener_log.notice("REMOVE -> pad=%d index=%d", pad, index);
			break;
		}
		case 0x03: // MOVE
		{
			const u8 old_pad = header[3];
			const u8 old_index = header[4];
			if (old_pad < 1 || old_pad > 3 || old_index >= 7)
			{
				dim_listener_log.error("Rejected MOVE source: pad=%d index=%d", old_pad, old_index);
				return;
			}

			if (!g_dimensionstoypad.temp_remove(old_index))
			{
				dim_listener_log.error("Ignored MOVE from empty source slot: pad=%d index=%d", old_pad, old_index);
				return;
			}

			std::this_thread::sleep_for(move_pickup_delay);
			g_dimensionstoypad.move_figure(pad, index, old_pad, old_index);
			dim_listener_log.notice("MOVE %d/%d -> %d/%d", old_pad, old_index, pad, index);
			break;
		}
		case 0x04: // GET_LED - return the current LED snapshot for the app's poll
		{
			const auto states = g_dimensionstoypad.get_led_states();
			const u8 serial = g_dimensionstoypad.get_led_serial();

			u8 response[3 + 3 * 9] = {};
			response[0] = 0x4C; // 'L' magic
			response[1] = serial;
			response[2] = 0x03; // region count
			for (usz i = 0; i < 3; ++i)
			{
				const usz off = 3 + i * 9;
				response[off + 0] = states[i].pad;
				response[off + 1] = states[i].mode;
				response[off + 2] = states[i].r;
				response[off + 3] = states[i].g;
				response[off + 4] = states[i].b;
				response[off + 5] = states[i].on_ms;
				response[off + 6] = states[i].off_ms;
				response[off + 7] = states[i].count;
				response[off + 8] = states[i].speed_ms;
			}
			if (!send_all(client, response, sizeof(response)))
				dim_listener_log.error("Failed to send GET_LED response");
			break;
		}
		default:
			dim_listener_log.error("Unknown command 0x%02x", cmd);
			break;
		}
	}

	void listener_run(u16 port)
	{
		const socket_t listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listen_sock == invalid_sock)
		{
			dim_listener_log.error("Could not create listener socket");
			return;
		}

		int reuse = 1;
		::setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if (::bind(listen_sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
			::listen(listen_sock, 4) != 0)
		{
			dim_listener_log.error("Could not bind/listen on 127.0.0.1:%d", port);
			close_sock(listen_sock);
			return;
		}

		g_listen_sock = listen_sock;
		dim_listener_log.success("Toypad listener active on 127.0.0.1:%d", port);

		while (g_listener_running)
		{
			const socket_t client = ::accept(listen_sock, nullptr, nullptr);
			if (client == invalid_sock)
				break; // socket closed by dimensions_listener_stop()
			handle_client(client);
			close_sock(client);
		}
	}
} // namespace

void dimensions_listener_start()
{
	if (g_listener_running.exchange(true))
		return; // already running (e.g. device re-created on game restart)

#ifdef _WIN32
	WSADATA wsa_data{};
	WSAStartup(MAKEWORD(2, 2), &wsa_data); // ref-counted, safe if already done
#endif

	g_listener_thread = std::thread(listener_run, listener_port());
#ifdef _WIN32
	g_picker_thread = std::thread(picker_watch_run);
#endif
}

void dimensions_listener_stop()
{
	if (!g_listener_running.exchange(false))
		return;

	if (const socket_t sock = g_listen_sock.exchange(invalid_sock); sock != invalid_sock)
		close_sock(sock); // unblocks accept()

	if (g_listener_thread.joinable())
		g_listener_thread.join();

#ifdef _WIN32
	if (g_picker_thread.joinable())
		g_picker_thread.join();
	WSACleanup();
#endif
}
