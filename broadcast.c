/* =============================================================================
// BROADcast
//
// Force IPv4 UDP broadcast on all network interfaces on Windows 7 and later.
//
// https://buymeacoff.ee/ubihazard
// -------------------------------------------------------------------------- */

#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <Winsock2.h>
#include <Windows.h>
#include <Mswsock.h>
#include <Ws2tcpip.h>
#include <Iphlpapi.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <stddef.h>
#include <signal.h>
#include <wchar.h>

/* -------------------------------------------------------------------------- */

#define numof(carr) (sizeof(carr) / sizeof(carr[0]))

/* -------------------------------------------------------------------------- */

#define APP_TITLE L"BROADcast"
#define APP_VERSION L"1.1"

#if BUF_SIZE == 0
/* Sizes less than around 512 bytes would likely result
// in data fragmentation and won't be broadcast properly.
// 65535 bytes is guaranteed to work always. */
#define BUF_SIZE 65535
#endif

#if (BUF_SIZE < 256 || BUF_SIZE > 0x10000)
#error "Invalid definition of `BUF_SIZE`"
#endif

#define METRIC_CHANGE_TRIES_MAX 5

/* -------------------------------------------------------------------------- */

#define IP_HEADER_SIZE 20
#define IP_ADDR_SRC_POS 12
#define IP_ADDR_DST_POS 16

#define UDP_HEADER_SIZE 8
#define UDP_LENGTH_POS 4
#define UDP_CHECKSUM_POS 6

/* -------------------------------------------------------------------------- */

static HANDLE evnt_stop;
static HANDLE evnt_read;
static HANDLE evnt_write;
static OVERLAPPED ovlp_read;
static OVERLAPPED ovlp_write;
static SOCKET sock_listen;
static ULONG addr_localhost;
static ULONG addr_broadcast;
static DWORD service_status;
static BOOL is_service;
static BOOL trace;
static BOOL fail;

/* -------------------------------------------------------------------------- */

static inline void set_text_color (int const color)
{
  SetConsoleTextAttribute (GetStdHandle (STD_OUTPUT_HANDLE), color);
}

static void msg_error (const wchar_t* const msg)
{
  if (is_service) return;
  set_text_color (4);
  _putws (msg);
  set_text_color (7);
}

/* -----------------------------------------------------------------------------
// We need to recompute the UDP packet checksum
// when we change its source address */
static void udp_chksum (unsigned char* const payload, DWORD const payload_sz
, DWORD const addr_src, DWORD const addr_dst)
{
  size_t sz = payload_sz;
  WORD* p = (WORD*)payload;

  /* Reset */
  DWORD chksum = 0;

  /* Compute data */
  while (sz > 1) {
    chksum += htons (*p++);
    sz -= 2;
  }

  /* Last byte of data */
  if (sz != 0) {
    chksum += *(BYTE*)p << 8;
  }

  /* Source address */
  chksum += addr_src >> 16;
  chksum += addr_src & 0xFFFF;

  /* Destination address */
  chksum += addr_dst >> 16;
  chksum += addr_dst & 0xFFFF;

  /* Protocol and payload size */
  chksum += htons (IPPROTO_UDP);
  chksum += htons (payload_sz);

  /* Write the new checksum */
  chksum = (chksum & 0xFFFF) + (chksum >> 16);
  *(WORD*)(payload + UDP_CHECKSUM_POS) = (WORD)~chksum;
}

/* -----------------------------------------------------------------------------
// By manipulating interface metric we can change the preferred route */
static int metric_update (const wchar_t* const iface, BOOL const manual)
{
  ULONG metric = 0;

  /* Obtain the list of Ethernet & Wi-Fi adapters */
  PIP_ADAPTER_ADDRESSES addresses = NULL, runner;
  DWORD addresses_sz = 0, tries = 0, result;

  do {
    result = GetAdaptersAddresses (AF_UNSPEC, GAA_FLAG_SKIP_UNICAST
    | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER
    , NULL, addresses, &addresses_sz);

    if (result == ERROR_BUFFER_OVERFLOW) {
      free (addresses);
      addresses = malloc (addresses_sz);
      if (addresses == NULL) {
        return FALSE;
      }
    } else if (result != NO_ERROR) {
      free (addresses);
      return FALSE;
    }

    ++tries;
  } while (result == ERROR_BUFFER_OVERFLOW
  && tries < METRIC_CHANGE_TRIES_MAX);

  if (tries == METRIC_CHANGE_TRIES_MAX) {
    free (addresses);
    return FALSE;
  }

  /* Look up the requested network interface */
  if (manual) {
    runner = addresses;

    while (runner != NULL)
    {
      if ((runner->IfType != IF_TYPE_ETHERNET_CSMACD)
      &&  (runner->IfType != IF_TYPE_IEEE80211)) {
        runner = runner->Next;
        continue;
      }

      if (_wcsicmp (iface, runner->FriendlyName) == 0) {
        /* Remember old metric */
        metric = runner->Ipv4Metric;
        break;
      }

      runner = runner->Next;
    }

    /* Couldn't find */
    if (metric == 0) {
      free (addresses);
      return FALSE;
    }
  }

  /* Update metric values */
  runner = addresses;

  while (runner != NULL) {
    if ((runner->IfType != IF_TYPE_ETHERNET_CSMACD)
    &&  (runner->IfType != IF_TYPE_IEEE80211)) {
      runner = runner->Next;
      continue;
    }

    if (_wcsicmp (iface, runner->FriendlyName) != 0) {
      MIB_IPINTERFACE_ROW iface_row;
      InitializeIpInterfaceEntry (&iface_row);
      iface_row.InterfaceLuid = runner->Luid;
      iface_row.Family = AF_INET;

      if (manual) {
        iface_row.UseAutomaticMetric = FALSE;
        iface_row.Metric = runner->Ipv4Metric + metric;
      } else {
        iface_row.UseAutomaticMetric = TRUE;
      }

      SetIpInterfaceEntry (&iface_row);
    }

    runner = runner->Next;
  }

  free (addresses);
  return TRUE;
}

/* -------------------------------------------------------------------------- */

BOOL already_stopped;

static void signal_handler (int signum)
{
  if (already_stopped) return;
  already_stopped = TRUE;
  if (trace) {
    set_text_color (3);
    wprintf (L"Exiting on signal: %d\n", signum);
    set_text_color (7);
  }
  if (!SetEvent (evnt_stop)) {
    if (trace) {
      set_text_color (4);
      wprintf (L"`SetEvent()` failed: %u\n", (unsigned)GetLastError());
      set_text_color (7);
    }
    /* Calling `WSACleanup()` from signal handler causes deadlock */
    ExitProcess (EXIT_FAILURE);
  }
}

static void broadcast_loop (void)
{
  unsigned char buf[BUF_SIZE];
  const char opt_broadcast = 1;

  SOCKADDR_IN sa_addr_dst = {0};
  sa_addr_dst.sin_family = AF_INET;
  sa_addr_dst.sin_addr.s_addr = addr_broadcast;

  sockaddr_gen sa_addr_broadcast = {0};
  sa_addr_broadcast.Address.sa_family = AF_INET;
  sa_addr_broadcast.AddressIn.sin_addr.s_addr = addr_broadcast;

  ULONG fwd_table_sz = 0;
  PMIB_IPFORWARDTABLE fwd_table = NULL;

  WSABUF wsa_buf = {0};
  wsa_buf.buf = (char*)buf;
  wsa_buf.len = sizeof(buf);

  DWORD code, flags;
  DWORD read_num, read_total, to_read;
  DWORD write_num, write_total;
  ULONG addr_src, addr_dst;

  read_total = 0;
  to_read = IP_HEADER_SIZE + UDP_HEADER_SIZE;

  while (TRUE) {
    flags = 0;
    code = WSARecv (sock_listen, &wsa_buf, 1u, &read_num, &flags
    , &ovlp_read, NULL);

    if (code == SOCKET_ERROR) {
      if (WSAGetLastError() != WSA_IO_PENDING) {
        msg_error (L"Error listening on the broadcast socket.");
        fail = TRUE;
        goto done;
      }

      HANDLE evnts_read[] = {evnt_read, evnt_stop};
      DWORD const wait = WSAWaitForMultipleEvents (numof(evnts_read), evnts_read
      , FALSE, INFINITE, FALSE);

      /* Ctrl+C */
      if (wait - WAIT_OBJECT_0 == 1) goto done;

      DWORD nul;
      WSAGetOverlappedResult (sock_listen, &ovlp_read, &read_num, FALSE, &nul);
    }

    /* Wait until we have a complete UDP datagram */
    read_total += read_num;

next_packet:
    if (read_total < to_read) {
      wsa_buf.buf += read_num;
      wsa_buf.len -= read_num;
      continue;
    }

#ifndef NDEBUG
    wprintf (L"[DEBUG] Source address: %.8X\n", (unsigned)ntohl(*(ULONG*)(buf + IP_ADDR_SRC_POS)));
    wprintf (L"[DEBUG] Destination address: %.8X\n", (unsigned)ntohl(*(ULONG*)(buf + IP_ADDR_DST_POS)));
    wprintf (L"[DEBUG] Source port: %u\n", (unsigned)ntohs(*(WORD*)(buf + IP_HEADER_SIZE)));
    wprintf (L"[DEBUG] Destination port: %u\n", (unsigned)ntohs(*(WORD*)(buf + IP_HEADER_SIZE + 2)));
    wprintf (L"[DEBUG] Size: %u\n", (unsigned)ntohs(*(WORD*)(buf + IP_HEADER_SIZE + UDP_LENGTH_POS)));
    wprintf (L"[DEBUG] Checksum: %x\n", (unsigned)ntohs(*(WORD*)(buf + IP_HEADER_SIZE + UDP_CHECKSUM_POS)));
#endif

    DWORD const packet_size = ntohs(*(WORD*)(buf + IP_HEADER_SIZE + UDP_LENGTH_POS));
    to_read = IP_HEADER_SIZE + packet_size;

    if (read_total < to_read) {
      wsa_buf.buf = (char*)buf + read_total;
      wsa_buf.len = sizeof(buf) - read_total;
      continue;
    }

    /* Get the packet addresses */
    addr_src = *(ULONG*)(buf + IP_ADDR_SRC_POS);
    addr_dst = *(ULONG*)(buf + IP_ADDR_DST_POS);

    /* Find out the preferred broadcast route */
    sockaddr_gen sa_addr_route = {0};

    if (WSAIoctl (sock_listen, SIO_ROUTING_INTERFACE_QUERY, &sa_addr_broadcast
    , sizeof(sa_addr_broadcast), &sa_addr_route, sizeof(sa_addr_route)
    , &flags, NULL, NULL) == SOCKET_ERROR) {
      if (!((WSAGetLastError() == WSAENETUNREACH) || (WSAGetLastError() == WSAEHOSTUNREACH)
      ||    (WSAGetLastError() == WSAENETDOWN))) {
        msg_error (L"Couldn't get the preferred broadcast route.");
        fail = TRUE;
        goto done;
      }
    }

    ULONG const addr_route = sa_addr_route.AddressIn.sin_addr.s_addr;

    /* Diagnostics */
    if (trace) {
      const int main_color = (addr_src == addr_route && addr_dst == addr_broadcast) ? 2 : 8;
      set_text_color (main_color);
      wprintf (L"Source: ");
      set_text_color (6);
      wprintf (L"%u.%u.%u.%u", addr_src & 0xFF, (addr_src >> 8) & 0xFF
      , (addr_src >> 16) & 0xFF, (addr_src >> 24) & 0xFF);
      set_text_color (main_color);
      wprintf (L" | Destination: ");
      set_text_color (6);
      wprintf (L"%u.%u.%u.%u", addr_dst & 0xFF, (addr_dst >> 8) & 0xFF
      , (addr_dst >> 16) & 0xFF, (addr_dst >> 24) & 0xFF);
      set_text_color (main_color);
      wprintf (L" | Preferred: ");
      set_text_color (6);
      wprintf (L"%u.%u.%u.%u", addr_route & 0xFF, (addr_route >> 8) & 0xFF
      , (addr_route >> 16) & 0xFF, (addr_route >> 24) & 0xFF);
      set_text_color (main_color);
      wprintf (L" | Size: ");
      set_text_color (5);
      wprintf (L"%u\n", packet_size);
      set_text_color (7);
    }

    /* Got broadcast packet from the preferred route? */
    if (addr_src == addr_route && addr_dst == addr_broadcast) {
      /* Get the forwarding table */
      int i = 0;

      while ((code = GetIpForwardTable (fwd_table, &fwd_table_sz
      , FALSE)) != NO_ERROR) {
        ++i;

        if (code == ERROR_INSUFFICIENT_BUFFER && i < METRIC_CHANGE_TRIES_MAX) {
          fwd_table = realloc (fwd_table, fwd_table_sz);
          continue;
        }

        msg_error (L"Error getting the forwarding table.");
        fail = TRUE;
        goto done;
      }

      /* Find other network interfaces to relay from */
      for (i = 0; i < fwd_table->dwNumEntries; ++i) {
        /* Only local routes with final destination */
        if (fwd_table->table[i].dwForwardType != MIB_IPROUTE_TYPE_DIRECT) continue;
        /* Netmask must be 255.255.255.255 */
        if (fwd_table->table[i].dwForwardMask != ULONG_MAX) continue;
        /* Destination must be 255.255.255.255 */
        if (fwd_table->table[i].dwForwardDest != addr_broadcast) continue;
        /* Local address must not be 0.0.0.0 */
        if (fwd_table->table[i].dwForwardNextHop == 0) continue;
        /* Local address must not be 127.0.0.1 */
        if (fwd_table->table[i].dwForwardNextHop == addr_localhost) continue;
        /* Local address must not be preferred route */
        if (fwd_table->table[i].dwForwardNextHop == addr_src) continue;

        /* Create the new source socket (not the preferred route) */
        ULONG const addr_src_new = fwd_table->table[i].dwForwardNextHop;
        SOCKET const sock_src_new = WSASocketW (AF_INET, SOCK_RAW, IPPROTO_UDP
        , NULL, 0, WSA_FLAG_OVERLAPPED);

        if (sock_src_new == INVALID_SOCKET) {
          msg_error (L"Couldn't create the new source socket.");
          goto skip_failed_iface;
        }

        /* Bind it to the next interface and send broadcast packet from it */
        SOCKADDR_IN sa_addr_src_new = {0};
        sa_addr_src_new.sin_family = AF_INET;
        sa_addr_src_new.sin_addr.s_addr = addr_src_new;

        if (bind (sock_src_new, (SOCKADDR*)&sa_addr_src_new
        , sizeof(sa_addr_src_new)) == SOCKET_ERROR) {
          msg_error (L"Couldn't bind to the new source socket.");
          goto skip_failed_iface;
        }

        if (setsockopt (sock_src_new, SOL_SOCKET, SO_BROADCAST
        , &opt_broadcast, sizeof(opt_broadcast)) == SOCKET_ERROR) {
          msg_error (L"`setsockopt()` failed on the new source socket.");
          goto skip_failed_iface;
        }

        /* Send the packet */
        wsa_buf.buf = (char*)(buf + IP_HEADER_SIZE);
        write_total = wsa_buf.len = packet_size;

        /* Recompute UDP header checksum */
        udp_chksum ((unsigned char*)wsa_buf.buf, packet_size
        , sa_addr_src_new.sin_addr.s_addr, sa_addr_dst.sin_addr.s_addr);

        while (TRUE) {
          code = WSASendTo (sock_src_new, &wsa_buf, 1u, &write_num, 0
          , (SOCKADDR*)&sa_addr_dst, sizeof(sa_addr_dst)
          , &ovlp_write, NULL);

          if (code == SOCKET_ERROR) {
            if (WSAGetLastError() != WSA_IO_PENDING) {
              set_text_color (4);
              wprintf (L"Error relaying packet to ");
              set_text_color (6);
              wprintf (L"%u.%u.%u.%u\n"
              ,  addr_src_new        & 0xFF
              , (addr_src_new >> 8)  & 0xFF
              , (addr_src_new >> 16) & 0xFF
              , (addr_src_new >> 24) & 0xFF);
              set_text_color (7);
              goto skip_failed_iface;
            }

            HANDLE evnts_write[] = {evnt_write, evnt_stop};
            DWORD const wait = WSAWaitForMultipleEvents (numof(evnts_write), evnts_write
            , FALSE, INFINITE, FALSE);

            /* Ctrl+C */
            if (wait - WAIT_OBJECT_0 == 1) {
              closesocket (sock_src_new);
              goto done;
            }

            DWORD nul;
            WSAGetOverlappedResult (sock_src_new, &ovlp_write, &write_num, FALSE, &nul);
          }

          write_total -= write_num;
          if (write_total == 0) break;
          wsa_buf.buf += write_num;
          wsa_buf.len -= write_num;
        }

        /* Diagnostics */
        if (trace) {
          wprintf (L"Relayed ");
          set_text_color (5);
          wprintf (L"%u", packet_size);
          set_text_color (7);
          wprintf (L" bytes to ");
          set_text_color (6);
          wprintf (L"%u.%u.%u.%u\n"
          ,  addr_src_new        & 0xFF
          , (addr_src_new >> 8)  & 0xFF
          , (addr_src_new >> 16) & 0xFF
          , (addr_src_new >> 24) & 0xFF);
          set_text_color (7);
        }

skip_failed_iface:
        closesocket (sock_src_new);
      }
    }

    read_total -= IP_HEADER_SIZE + packet_size;
    to_read = IP_HEADER_SIZE + UDP_HEADER_SIZE;
    memmove (buf, buf + IP_HEADER_SIZE + packet_size, read_total);
    wsa_buf.buf = (char*)buf;
    wsa_buf.len = sizeof(buf);
    read_num = read_total;
    goto next_packet;
  }

done:
  HeapFree (GetProcessHeap(), 0, fwd_table);
}

static void svc_report (DWORD, DWORD, DWORD);

static void broadcast_start (void)
{
  /* Initialize Winsock */
  WORD const wsa_ver = MAKEWORD (2, 2);
  WSADATA wsa_data = {0};

  if (WSAStartup (wsa_ver, &wsa_data) != 0) {
    msg_error (L"Error initializing WinSock.");
    fail = TRUE;
    return;
  }

  /* Initialize addresses */
  addr_localhost = inet_addr ("127.0.0.1");
  addr_broadcast = inet_addr ("255.255.255.255");

  /* Bind to localhost */
  sock_listen = WSASocketW (AF_INET, SOCK_RAW, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);

  if (sock_listen == INVALID_SOCKET) {
    msg_error (L"Error creating the listening socket.");
    WSACleanup();
    fail = TRUE;
    return;
  }

  /* We are listening on a raw socket */
  SOCKADDR_IN sa_localhost = {0};
  sa_localhost.sin_family = AF_INET;
  sa_localhost.sin_addr.s_addr = addr_localhost;

  if (bind (sock_listen, (SOCKADDR*)&sa_localhost
  , sizeof(sa_localhost)) == SOCKET_ERROR) {
    msg_error (L"Error binding on the listening socket.");
    closesocket (sock_listen);
    WSACleanup();
    fail = TRUE;
    return;
  }

  /* Handle Ctrl+C */
  signal (SIGINT,  signal_handler);
  signal (SIGTERM, signal_handler);
  signal (SIGABRT, signal_handler);

  /* Create structures for overlapped I/O */
  evnt_stop = CreateEventW (NULL, TRUE, FALSE, NULL);
  evnt_read = CreateEventW (NULL, TRUE, FALSE, NULL);
  evnt_write = CreateEventW (NULL, TRUE, FALSE, NULL);

  if (evnt_stop == NULL || evnt_read == NULL || evnt_write == NULL) {
    msg_error (L"Error creating asynchronous events.");
    closesocket (sock_listen);
    WSACleanup();
    fail = TRUE;
    return;
  }

  ovlp_read.hEvent = evnt_read;
  ovlp_write.hEvent = evnt_write;

  /* Enter the broadcast loop */
  if (trace) {
    set_text_color (3);
    wprintf (APP_TITLE L" " APP_VERSION);
    set_text_color (7);
    wprintf (L" is ready.\n");
    set_text_color (6);
    _putws (L"https://buymeacoff.ee/ubihazard\n");
    set_text_color (7);
  }
  service_status = SERVICE_RUNNING;
  svc_report (SERVICE_RUNNING, NO_ERROR, 0);
  broadcast_loop();

  /* Cleanup */
  CloseHandle (evnt_stop);
  CloseHandle (evnt_read);
  CloseHandle (evnt_write);
  closesocket (sock_listen);
  WSACleanup();
}

/* -------------------------------------------------------------------------- */

static BOOL is_admin (void)
{
  BOOL ret = FALSE;
  HANDLE token = NULL;
  if (OpenProcessToken (GetCurrentProcess(), TOKEN_QUERY, &token)) {
    TOKEN_ELEVATION elev;
    DWORD size = sizeof(TOKEN_ELEVATION);
    if (GetTokenInformation (token, TokenElevation, &elev, sizeof(elev), &size)) {
      ret = elev.TokenIsElevated;
    }
  }
  if (token != NULL) {
    CloseHandle (token);
  }
  return ret;
}

/* -------------------------------------------------------------------------- */

SERVICE_STATUS        svc_status;
SERVICE_STATUS_HANDLE svc_status_hndl;

static BOOL svc_install (void)
{
  wchar_t path[MAX_PATH];
  if (!GetModuleFileNameW (NULL, path, MAX_PATH)) return FALSE;
  SC_HANDLE const scman = OpenSCManagerW (NULL, NULL, SC_MANAGER_ALL_ACCESS);
  if (scman == NULL) return FALSE;
  SC_HANDLE const svc = CreateServiceW (
    scman,
    APP_TITLE,
    APP_TITLE,
    SERVICE_ALL_ACCESS,
    SERVICE_WIN32_OWN_PROCESS,
    SERVICE_DEMAND_START,
    SERVICE_ERROR_NORMAL,
    path,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
  );
  if (svc == NULL) {
    CloseServiceHandle (scman);
    return FALSE;
  }
  SERVICE_DESCRIPTIONW desc = {
    .lpDescription = L"Force IPv4 UDP broadcast on all network interfaces. "
    L"https://buymeacoff.ee/ubihazard"
  };
  ChangeServiceConfig2W (svc, SERVICE_CONFIG_DESCRIPTION, &desc);
  CloseServiceHandle (svc);
  CloseServiceHandle (scman);
  return TRUE;
}

static BOOL svc_uninstall (void)
{
  SC_HANDLE const scman = OpenSCManagerW (NULL, NULL, SC_MANAGER_ALL_ACCESS);
  if (scman == NULL) return FALSE;
  SC_HANDLE const svc = OpenServiceW (scman, APP_TITLE, DELETE);
  if (svc == NULL) {
    CloseServiceHandle (scman);
    return FALSE;
  }
  BOOL ret;
  if (DeleteService (svc)) ret = TRUE;
  else ret = FALSE;
  CloseServiceHandle (svc);
  CloseServiceHandle (scman);
  return ret;
}

static void svc_report (DWORD const state, DWORD const exit_code, DWORD const wait_hint)
{
  static DWORD check_point = 0;
  if (!is_service) return;
  svc_status.dwCurrentState = state;
  svc_status.dwWin32ExitCode = exit_code;
  svc_status.dwWaitHint = wait_hint;
  if (state == SERVICE_START_PENDING) svc_status.dwControlsAccepted = 0;
  else svc_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
  if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) svc_status.dwCheckPoint = 0;
  else svc_status.dwCheckPoint = ++check_point;
  SetServiceStatus (svc_status_hndl, &svc_status);
}

__stdcall static void svc_handler (DWORD const code)
{
  if (code == SERVICE_CONTROL_INTERROGATE) {
    if (service_status != SERVICE_STOPPED) svc_report (service_status, NO_ERROR, 0);
    else svc_report (SERVICE_STOPPED, fail ? EXIT_FAILURE : EXIT_SUCCESS, 0);
    return;
  }
  if (code == SERVICE_CONTROL_STOP) {
    service_status = SERVICE_STOP_PENDING;
    svc_report (SERVICE_STOP_PENDING, NO_ERROR, 1000);
    SetEvent (evnt_stop);
    return;
  }
}

static void WINAPI svc_main (DWORD const argc, LPWSTR* const argv)
{
  trace = FALSE;
  service_status = SERVICE_STOPPED;
  svc_status_hndl = RegisterServiceCtrlHandlerW (APP_TITLE, svc_handler);
  if (!svc_status_hndl) return;
  svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  svc_status.dwServiceSpecificExitCode = 0;
  service_status = SERVICE_START_PENDING;
  svc_report (SERVICE_START_PENDING, NO_ERROR, 1000);
  broadcast_start();
  service_status = SERVICE_STOPPED;
  svc_report (SERVICE_STOPPED, fail ? EXIT_FAILURE : EXIT_SUCCESS, 0);
}

/* ========================================================================== */

int wmain (int argc, wchar_t** argv)
{
  /* Determine if running as a Windows service */
  is_service = TRUE;

  if (argc > 1) {
    if (_wcsicmp (argv[1], L"install") == 0) {
      return svc_install() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (_wcsicmp (argv[1], L"uninstall") == 0) {
      return svc_uninstall() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  }

  SERVICE_TABLE_ENTRY svc_table[] = {
    {APP_TITLE, (LPSERVICE_MAIN_FUNCTION)svc_main},
    {NULL, NULL}
  };

  if (!StartServiceCtrlDispatcher (svc_table)) {
    if (GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
      /* Could not start as a service */
      return EXIT_FAILURE;
    }
  }

  /* Run as console application */
  is_service = FALSE;
  SetConsoleTitleW (APP_TITLE L" " APP_VERSION);

  /* Windows 7+ is recommended */
  OSVERSIONINFOEX osvi = {
    .dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX),
    .dwMajorVersion = 6,
    .dwMinorVersion = 1
  };
  DWORDLONG dwlConditionMask = 0;
  VER_SET_CONDITION (dwlConditionMask, VER_MAJORVERSION, VER_GREATER_EQUAL);
  VER_SET_CONDITION (dwlConditionMask, VER_MINORVERSION, VER_GREATER_EQUAL);
  if (VerifyVersionInfoW (&osvi, VER_MAJORVERSION | VER_MINORVERSION
  | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, dwlConditionMask) == 0) {
    msg_error (L"Warning: this program isn't guaranteed to work on Windows Vista and earlier.\n");
  }

  /* Get command line arguments (order matters) */
  const wchar_t* const app = PathFindFileNameW (argv[0]);
  argc--;
  argv++;
  if (!argc) goto usage;

  while (argc) {
    if (_wcsicmp (L"-i", argv[0]) == 0) {
      /* Change metric */
      argc--;
      argv++;
      if (!argc) {
        fail = TRUE;
        goto usage;
      }

      BOOL const manual = argc > 1 && _wcsicmp (L"-m", argv[1]) == 0;
      BOOL const ret = metric_update (argv[0], manual);
      if (!ret) {
        msg_error (L"Couldn't change the metric configuration.");
        return EXIT_FAILURE;
      }

      argc -= 1 + manual;
      argv += 1 + manual;
    } else if (_wcsicmp (L"-b", argv[0]) == 0) {
      /* Broadcast */
      argc--;
      argv++;

      if (argc > 1) {
        fail = TRUE;
        goto usage;
      }
      if (argc == 1) {
        trace = _wcsicmp (L"-d", argv[0]) == 0;
        if (!trace) {
          fail = TRUE;
          goto usage;
        }
      }

      broadcast_start();
      break;
    } else if (_wcsicmp (L"-h", argv[0]) == 0) {
      /* Help */
usage:
      set_text_color (3);
      _putws (L"BROADcast" L" " APP_VERSION);
      set_text_color (6);
      _putws (L"https://buymeacoff.ee/ubihazard\n");
      set_text_color (7);
      _putws (L"Force IPv4 UDP broadcast on all network interfaces.");
      if (!is_admin()) {
        msg_error (L"This program must be run with administrator privileges.");
      }
      wprintf (L"\n"
"%s -i <interface> [-m]:\n"
"\n"
"Make the specified interface a preferred route (with `-m`)\n"
"by manipulating the metric value of all network\n"
"interfaces available on the system.\n"
"\n"
"If `-m` option is omitted, all metric changes\n"
"are reverted to automatic system-managed values.\n"
"\n"
"%s -b [-d]:\n"
"\n"
"Start IPv4 UDP broadcast relaying.\n"
"\n"
"The `-d` option enables diagnostic messages to help verify\n"
"that broadcast is actually working.\n"
"\n"
"Options can be combined into a single command line,\n"
"but the broadcast (`-b`) option must be specified last,\n"
"or the metric changes will be ignored.\n"
      , app, app);
      goto done;
    } else {
      fail = TRUE;
      goto usage;
    }
  }

done:
  return fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
