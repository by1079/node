/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <direct.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "uv.h"
#include "internal.h"

#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>


/*
 * Max title length; the only thing MSDN tells us about the maximum length
 * of the console title is that it is smaller than 64K. However in practice
 * it is much smaller, and there is no way to figure out what the exact length
 * of the title is or can be, at least not on XP. To make it even more
 * annoying, GetConsoleTitle failes when the buffer to be read into is bigger
 * than the actual maximum length. So we make a conservative guess here;
 * just don't put the novel you're writing in the title, unless the plot
 * survives truncation.
 */
#define MAX_TITLE_LENGTH 8192


static char *process_title;
static uv_once_t uv_process_title_init_guard_ = UV_ONCE_INIT;
static CRITICAL_SECTION process_title_lock;

int uv_utf16_to_utf8(const wchar_t* utf16Buffer, size_t utf16Size,
    char* utf8Buffer, size_t utf8Size) {
  return WideCharToMultiByte(CP_UTF8,
                             0,
                             utf16Buffer,
                             utf16Size,
                             utf8Buffer,
                             utf8Size,
                             NULL,
                             NULL);
}


int uv_utf8_to_utf16(const char* utf8Buffer, wchar_t* utf16Buffer,
    size_t utf16Size) {
  return MultiByteToWideChar(CP_UTF8,
                             0,
                             utf8Buffer,
                             -1,
                             utf16Buffer,
                             utf16Size);
}


int uv_exepath(char* buffer, size_t* size) {
  int retVal;
  size_t utf16Size;
  wchar_t* utf16Buffer;

  if (!buffer || !size) {
    return -1;
  }

  utf16Buffer = (wchar_t*)malloc(sizeof(wchar_t) * *size);
  if (!utf16Buffer) {
    retVal = -1;
    goto done;
  }

  /* Get the path as UTF-16 */
  utf16Size = GetModuleFileNameW(NULL, utf16Buffer, *size - 1);
  if (utf16Size <= 0) {
    /* uv__set_sys_error(loop, GetLastError()); */
    retVal = -1;
    goto done;
  }

  utf16Buffer[utf16Size] = L'\0';

  /* Convert to UTF-8 */
  *size = uv_utf16_to_utf8(utf16Buffer, utf16Size, buffer, *size);
  if (!*size) {
    /* uv__set_sys_error(loop, GetLastError()); */
    retVal = -1;
    goto done;
  }

  buffer[*size] = '\0';
  retVal = 0;

done:
  if (utf16Buffer) {
    free(utf16Buffer);
  }

  return retVal;
}


uv_err_t uv_cwd(char* buffer, size_t size) {
  uv_err_t err;
  size_t utf8Size;
  wchar_t* utf16Buffer = NULL;

  if (!buffer || !size) {
    err.code = UV_EINVAL;
    goto done;
  }

  utf16Buffer = (wchar_t*)malloc(sizeof(wchar_t) * size);
  if (!utf16Buffer) {
    err.code = UV_ENOMEM;
    goto done;
  }

  if (!_wgetcwd(utf16Buffer, size - 1)) {
    err = uv__new_sys_error(_doserrno);
    goto done;
  }

  utf16Buffer[size - 1] = L'\0';

  /* Convert to UTF-8 */
  utf8Size = uv_utf16_to_utf8(utf16Buffer, -1, buffer, size);
  if (utf8Size == 0) {
    err = uv__new_sys_error(GetLastError());
    goto done;
  }

  buffer[utf8Size] = '\0';
  err = uv_ok_;

done:
  if (utf16Buffer) {
    free(utf16Buffer);
  }

  return err;
}


uv_err_t uv_chdir(const char* dir) {
  uv_err_t err;
  wchar_t* utf16Buffer = NULL;
  size_t utf16Size;

  if (!dir) {
    err.code = UV_EINVAL;
    goto done;
  }

  utf16Size = uv_utf8_to_utf16(dir, NULL, 0);
  if (!utf16Size) {
    err = uv__new_sys_error(GetLastError());
    goto done;
  }

  utf16Buffer = (wchar_t*)malloc(sizeof(wchar_t) * utf16Size);
  if (!utf16Buffer) {
    err.code = UV_ENOMEM;
    goto done;
  }

  if (!uv_utf8_to_utf16(dir, utf16Buffer, utf16Size)) {
    err = uv__new_sys_error(GetLastError());
    goto done;
  }

  if (_wchdir(utf16Buffer) == -1) {
    err = uv__new_sys_error(_doserrno);
    goto done;
  }

  err = uv_ok_;

done:
  if (utf16Buffer) {
    free(utf16Buffer);
  }

  return err;
}


void uv_loadavg(double avg[3]) {
  /* Can't be implemented */
  avg[0] = avg[1] = avg[2] = 0;
}


uint64_t uv_get_free_memory(void) {
  MEMORYSTATUSEX memory_status;
  memory_status.dwLength = sizeof(memory_status);

  if(!GlobalMemoryStatusEx(&memory_status))
  {
     return -1;
  }

  return (uint64_t)memory_status.ullAvailPhys;
}


uint64_t uv_get_total_memory(void) {
  MEMORYSTATUSEX memory_status;
  memory_status.dwLength = sizeof(memory_status);

  if(!GlobalMemoryStatusEx(&memory_status))
  {
    return -1;
  }

  return (uint64_t)memory_status.ullTotalPhys;
}


int uv_parent_pid() {
  int parent_pid = -1;
  HANDLE handle;
  PROCESSENTRY32 pe;
  int current_pid = GetCurrentProcessId();

  pe.dwSize = sizeof(PROCESSENTRY32);
  handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

  if (Process32First(handle, &pe)) {
    do {
      if (pe.th32ProcessID == current_pid) {
        parent_pid = pe.th32ParentProcessID;
        break;
      }
    } while( Process32Next(handle, &pe));
  }

  CloseHandle(handle);
  return parent_pid;
}


char** uv_setup_args(int argc, char** argv) {
  return argv;
}


static void uv_process_title_init(void) {
  InitializeCriticalSection(&process_title_lock);
}


uv_err_t uv_set_process_title(const char* title) {
  uv_err_t err;
  int length;
  wchar_t* title_w = NULL;

  uv_once(&uv_process_title_init_guard_, uv_process_title_init);

  /* Find out how big the buffer for the wide-char title must be */
  length = uv_utf8_to_utf16(title, NULL, 0);
  if (!length) {
    err = uv__new_sys_error(GetLastError());
    goto done;
  }

  /* Convert to wide-char string */
  title_w = (wchar_t*)malloc(sizeof(wchar_t) * length);
  if (!title_w) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  length = uv_utf8_to_utf16(title, title_w, length);
  if (!length) {
    err = uv__new_sys_error(GetLastError());
    goto done;
  };

  /* If the title must be truncated insert a \0 terminator there */
  if (length > MAX_TITLE_LENGTH) {
    title_w[MAX_TITLE_LENGTH - 1] = L'\0';
  }

  if (!SetConsoleTitleW(title_w)) {
    err = uv__new_sys_error(GetLastError());
    goto done;
  }

  EnterCriticalSection(&process_title_lock);
  free(process_title);
  process_title = strdup(title);
  LeaveCriticalSection(&process_title_lock);

  err = uv_ok_;

done:
  free(title_w);
  return err;
}


static int uv__get_process_title() {
  wchar_t title_w[MAX_TITLE_LENGTH];
  int length;

  if (!GetConsoleTitleW(title_w, sizeof(title_w) / sizeof(WCHAR))) {
    return -1;
  }

  /* Find out what the size of the buffer is that we need */
  length = uv_utf16_to_utf8(title_w, -1, NULL, 0);
  if (!length) {
    return -1;
  }

  assert(!process_title);
  process_title = (char*)malloc(length);
  if (!process_title) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  /* Do utf16 -> utf8 conversion here */
  if (!uv_utf16_to_utf8(title_w, -1, process_title, length)) {
    free(process_title);
    return -1;
  }

  return 0;
}


uv_err_t uv_get_process_title(char* buffer, size_t size) {
  uv_once(&uv_process_title_init_guard_, uv_process_title_init);

  EnterCriticalSection(&process_title_lock);
  /*
   * If the process_title was never read before nor explicitly set,
   * we must query it with getConsoleTitleW
   */
  if (!process_title && uv__get_process_title() == -1) {
    return uv__new_sys_error(GetLastError());
  }

  assert(process_title);
  strncpy(buffer, process_title, size);
  LeaveCriticalSection(&process_title_lock);

  return uv_ok_;
}


uv_err_t uv_resident_set_memory(size_t* rss) {
  HANDLE current_process;
  PROCESS_MEMORY_COUNTERS pmc;

  current_process = GetCurrentProcess();

  if (!GetProcessMemoryInfo(current_process, &pmc, sizeof(pmc))) {
    return uv__new_sys_error(GetLastError());
  }

  *rss = pmc.WorkingSetSize;

  return uv_ok_;
}


uv_err_t uv_uptime(double* uptime) {
  BYTE stack_buffer[4096];
  BYTE* malloced_buffer = NULL;
  BYTE* buffer = (BYTE*) stack_buffer;
  size_t buffer_size = sizeof(stack_buffer);
  DWORD data_size;

  PERF_DATA_BLOCK* data_block;
  PERF_OBJECT_TYPE* object_type;
  PERF_COUNTER_DEFINITION* counter_definition;

  DWORD i;

  for (;;) {
    LONG result;

    data_size = (DWORD) buffer_size;
    result = RegQueryValueExW(HKEY_PERFORMANCE_DATA,
                              L"2",
                              NULL,
                              NULL,
                              buffer,
                              &data_size);
    if (result == ERROR_SUCCESS) {
      break;
    } else if (result != ERROR_MORE_DATA) {
      *uptime = 0;
      return uv__new_sys_error(result);
    }

    free(malloced_buffer);

    buffer_size *= 2;
    /* Don't let the buffer grow infinitely. */
    if (buffer_size > 1 << 20) {
      goto internalError;
    }

    buffer = malloced_buffer = (BYTE*) malloc(buffer_size);
    if (malloced_buffer == NULL) {
      *uptime = 0;
      return uv__new_artificial_error(UV_ENOMEM);
    }
  }

  if (data_size < sizeof(*data_block))
    goto internalError;

  data_block = (PERF_DATA_BLOCK*) buffer;

  if (wmemcmp(data_block->Signature, L"PERF", 4) != 0)
    goto internalError;

  if (data_size < data_block->HeaderLength + sizeof(*object_type))
    goto internalError;

  object_type = (PERF_OBJECT_TYPE*) (buffer + data_block->HeaderLength);

  if (object_type->NumInstances != PERF_NO_INSTANCES)
    goto internalError;

  counter_definition = (PERF_COUNTER_DEFINITION*) (buffer +
      data_block->HeaderLength + object_type->HeaderLength);
  for (i = 0; i < object_type->NumCounters; i++) {
    if ((BYTE*) counter_definition + sizeof(*counter_definition) >
        buffer + data_size) {
      break;
    }

    if (counter_definition->CounterNameTitleIndex == 674 &&
        counter_definition->CounterSize == sizeof(uint64_t)) {
      if (counter_definition->CounterOffset + sizeof(uint64_t) > data_size ||
          !(counter_definition->CounterType & PERF_OBJECT_TIMER)) {
        goto internalError;
      } else {
        BYTE* address = (BYTE*) object_type + object_type->DefinitionLength +
                        counter_definition->CounterOffset;
        uint64_t value = *((uint64_t*) address);
        *uptime = (double) (object_type->PerfTime.QuadPart - value) /
                  (double) object_type->PerfFreq.QuadPart;
        free(malloced_buffer);
        return uv_ok_;
      }
    }

    counter_definition = (PERF_COUNTER_DEFINITION*)
        ((BYTE*) counter_definition + counter_definition->ByteLength);
  }

  /* If we get here, the uptime value was not found. */
  free(malloced_buffer);
  *uptime = 0;
  return uv__new_artificial_error(UV_ENOSYS);

 internalError:
  free(malloced_buffer);
  *uptime = 0;
  return uv__new_artificial_error(UV_EIO);
}


uv_err_t uv_cpu_info(uv_cpu_info_t** cpu_infos, int* count) {
  uv_err_t err;
  char key[128];
  HKEY processor_key = NULL;
  DWORD cpu_speed = 0;
  DWORD cpu_speed_length = sizeof(cpu_speed);
  char cpu_brand[256];
  DWORD cpu_brand_length = sizeof(cpu_brand);
  SYSTEM_INFO system_info;
  uv_cpu_info_t* cpu_info;
  unsigned int i;

  GetSystemInfo(&system_info);

  *cpu_infos = (uv_cpu_info_t*)malloc(system_info.dwNumberOfProcessors *
    sizeof(uv_cpu_info_t));
  if (!(*cpu_infos)) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  *count = 0;

  for (i = 0; i < system_info.dwNumberOfProcessors; i++) {
    _snprintf(key, sizeof(key), "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%d", i);

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE,
        &processor_key) != ERROR_SUCCESS) {
      if (i == 0) {
        err = uv__new_sys_error(GetLastError());
        goto done;
      }

      continue;
    }

    if (RegQueryValueEx(processor_key, "~MHz", NULL, NULL,
                        (LPBYTE)&cpu_speed, &cpu_speed_length)
                        != ERROR_SUCCESS) {
      err = uv__new_sys_error(GetLastError());
      goto done;
    }

    if (RegQueryValueEx(processor_key, "ProcessorNameString", NULL, NULL,
                        (LPBYTE)&cpu_brand, &cpu_brand_length)
                        != ERROR_SUCCESS) {
      err = uv__new_sys_error(GetLastError());
      goto done;
    }

    RegCloseKey(processor_key);
    processor_key = NULL;

    cpu_info = &(*cpu_infos)[i];

    /* $TODO: find times on windows */
    cpu_info->cpu_times.user = 0;
    cpu_info->cpu_times.nice = 0;
    cpu_info->cpu_times.sys = 0;
    cpu_info->cpu_times.idle = 0;
    cpu_info->cpu_times.irq = 0;

    cpu_info->model = strdup(cpu_brand);
    cpu_info->speed = cpu_speed;

    (*count)++;
  }

  err = uv_ok_;

done:
  if (processor_key) {
    RegCloseKey(processor_key);
  }

  if (err.code != UV_OK) {
    free(*cpu_infos);
    *cpu_infos = NULL;
    *count = 0;
  }

  return err;
}


void uv_free_cpu_info(uv_cpu_info_t* cpu_infos, int count) {
  int i;

  for (i = 0; i < count; i++) {
    free(cpu_infos[i].model);
  }

  free(cpu_infos);
}


uv_err_t uv_interface_addresses(uv_interface_address_t** addresses,
    int* count) {
  unsigned long size = 0;
  IP_ADAPTER_ADDRESSES* adapter_addresses;
  IP_ADAPTER_ADDRESSES* adapter_address;
  uv_interface_address_t* address;
  struct sockaddr* sock_addr;
  int length;
  char* name;
  /* Use IP_ADAPTER_UNICAST_ADDRESS_XP to retain backwards compatibility */
  /* with Windows XP */
  IP_ADAPTER_UNICAST_ADDRESS_XP* unicast_address;

  if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &size)
      != ERROR_BUFFER_OVERFLOW) {
    return uv__new_sys_error(GetLastError());
  }

  adapter_addresses = (IP_ADAPTER_ADDRESSES*)malloc(size);
  if (!adapter_addresses) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, adapter_addresses, &size)
      != ERROR_SUCCESS) {
    return uv__new_sys_error(GetLastError());
  }

  /* Count the number of interfaces */
  *count = 0;

  for (adapter_address = adapter_addresses;
       adapter_address != NULL;
       adapter_address = adapter_address->Next) {
    unicast_address = (IP_ADAPTER_UNICAST_ADDRESS_XP*)
                      adapter_address->FirstUnicastAddress;
    while (unicast_address) {
      (*count)++;
      unicast_address = unicast_address->Next;
    }
  }

  *addresses = (uv_interface_address_t*)
    malloc(*count * sizeof(uv_interface_address_t));
  if (!(*addresses)) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  address = *addresses;

  for (adapter_address = adapter_addresses;
       adapter_address != NULL;
       adapter_address = adapter_address->Next) {
    name = NULL;
    unicast_address = (IP_ADAPTER_UNICAST_ADDRESS_XP*)
                      adapter_address->FirstUnicastAddress;

    while (unicast_address) {
      sock_addr = unicast_address->Address.lpSockaddr;
      if (sock_addr->sa_family == AF_INET6) {
        address->address.address6 = *((struct sockaddr_in6 *)sock_addr);
      } else {
        address->address.address4 = *((struct sockaddr_in *)sock_addr);
      }

      address->is_internal =
        adapter_address->IfType == IF_TYPE_SOFTWARE_LOOPBACK ? 1 : 0;

      if (!name) {
        /* Convert FriendlyName to utf8 */
        length = uv_utf16_to_utf8(adapter_address->FriendlyName, -1, NULL, 0);
        if (length) {
          name = (char*)malloc(length);
          if (!name) {
            uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
          }

          if (!uv_utf16_to_utf8(adapter_address->FriendlyName, -1, name,
              length)) {
            free(name);
            name = NULL;
          }
        }
      }

      assert(name);
      address->name = name;

      unicast_address = unicast_address->Next;
      address++;
    }
  }

  free(adapter_addresses);

  return uv_ok_;
}


void uv_free_interface_addresses(uv_interface_address_t* addresses,
    int count) {
  int i;
  char* freed_name = NULL;

  for (i = 0; i < count; i++) {
    if (freed_name != addresses[i].name) {
      freed_name = addresses[i].name;
      free(freed_name);
    }
  }

  free(addresses);
}


void uv_filetime_to_time_t(FILETIME* file_time, time_t* stat_time) {
  FILETIME local_time;
  SYSTEMTIME system_time;
  struct tm time;

  if ((file_time->dwLowDateTime || file_time->dwHighDateTime) &&
      FileTimeToLocalFileTime(file_time, &local_time)         &&
      FileTimeToSystemTime(&local_time, &system_time)) {
    time.tm_year = system_time.wYear - 1900;
    time.tm_mon = system_time.wMonth - 1;
    time.tm_mday = system_time.wDay;
    time.tm_hour = system_time.wHour;
    time.tm_min = system_time.wMinute;
    time.tm_sec = system_time.wSecond;
    time.tm_isdst = -1;

    *stat_time = mktime(&time);
  } else {
    *stat_time = 0;
  }
}
