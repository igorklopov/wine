/*
 * msvcrt.dll file functions
 *
 * Copyright 1996,1998 Marcus Meissner
 * Copyright 1996 Jukka Iivonen
 * Copyright 1997,2000 Uwe Bonnes
 * Copyright 2000 Jon Griffiths
 */
#include <time.h>
#include <stdio.h>
#include <unistd.h>

#include "ntddk.h"
#include "msvcrt.h"
#include "ms_errno.h"

#include "wine/unicode.h"
#include "msvcrt/direct.h"
#include "msvcrt/fcntl.h"
#include "msvcrt/io.h"
#include "msvcrt/stdio.h"
#include "msvcrt/stdlib.h"
#include "msvcrt/string.h"
#include "msvcrt/sys/stat.h"
#include "msvcrt/sys/utime.h"
#include "msvcrt/time.h"

DEFAULT_DEBUG_CHANNEL(msvcrt);

/* for stat mode, permissions apply to all,owner and group */
#define MSVCRT_S_IREAD  (_S_IREAD  | (_S_IREAD  >> 3) | (_S_IREAD  >> 6))
#define MSVCRT_S_IWRITE (_S_IWRITE | (_S_IWRITE >> 3) | (_S_IWRITE >> 6))
#define MSVCRT_S_IEXEC  (_S_IEXEC  | (_S_IEXEC  >> 3) | (_S_IEXEC  >> 6))

/* _access() bit flags FIXME: incomplete */
#define W_OK      2


/* FIXME: Make this dynamic */
#define MSVCRT_MAX_FILES 257

HANDLE MSVCRT_handles[MSVCRT_MAX_FILES];
MSVCRT_FILE* MSVCRT_files[MSVCRT_MAX_FILES];
int  MSVCRT_flags[MSVCRT_MAX_FILES];
char *MSVCRT_tempfiles[MSVCRT_MAX_FILES];
MSVCRT_FILE MSVCRT__iob[3];
#define MSVCRT_stdin       (MSVCRT__iob+STDIN_FILENO)
#define MSVCRT_stdout      (MSVCRT__iob+STDOUT_FILENO)
#define MSVCRT_stderr      (MSVCRT__iob+STDERR_FILENO)

static int MSVCRT_fdstart = 3; /* first unallocated fd */
static int MSVCRT_fdend = 3; /* highest allocated fd */

/* INTERNAL: process umask */
static int MSVCRT_umask = 0;

/* INTERNAL: Static buffer for temp file name */
static char MSVCRT_tmpname[MAX_PATH];

static const unsigned int EXE = 'e' << 16 | 'x' << 8 | 'e';
static const unsigned int BAT = 'b' << 16 | 'a' << 8 | 't';
static const unsigned int CMD = 'c' << 16 | 'm' << 8 | 'd';
static const unsigned int COM = 'c' << 16 | 'o' << 8 | 'm';

#define TOUL(x) (ULONGLONG)((WCHAR)L##x)
static const ULONGLONG WCEXE = TOUL('e') << 32 | TOUL('x') << 16 | TOUL('e');
static const ULONGLONG WCBAT = TOUL('b') << 32 | TOUL('a') << 16 | TOUL('t');
static const ULONGLONG WCCMD = TOUL('c') << 32 | TOUL('m') << 16 | TOUL('d');
static const ULONGLONG WCCOM = TOUL('c') << 32 | TOUL('o') << 16 | TOUL('m');

extern CRITICAL_SECTION MSVCRT_file_cs;
#define LOCK_FILES     EnterCriticalSection(&MSVCRT_file_cs)
#define UNLOCK_FILES   LeaveCriticalSection(&MSVCRT_file_cs)


/* INTERNAL: Get the HANDLE for a fd */
static HANDLE msvcrt_fdtoh(int fd)
{
  if (fd < 0 || fd >= MSVCRT_fdend ||
      MSVCRT_handles[fd] == INVALID_HANDLE_VALUE)
  {
    WARN(":fd (%d) - no handle!\n",fd);
    SET_THREAD_VAR(doserrno,0);
    SET_THREAD_VAR(errno,MSVCRT_EBADF);
   return INVALID_HANDLE_VALUE;
  }
  return MSVCRT_handles[fd];
}

/* INTERNAL: free a file entry fd */
static void msvcrt_free_fd(int fd)
{
  MSVCRT_handles[fd] = INVALID_HANDLE_VALUE;
  MSVCRT_files[fd] = 0;
  MSVCRT_flags[fd] = 0;
  TRACE(":fd (%d) freed\n",fd);
  if (fd < 3)
    return; /* dont use 0,1,2 for user files */
  if (fd == MSVCRT_fdend - 1)
    MSVCRT_fdend--;
  if (fd < MSVCRT_fdstart)
    MSVCRT_fdstart = fd;
}

/* INTERNAL: Allocate an fd slot from a Win32 HANDLE */
static int msvcrt_alloc_fd(HANDLE hand, int flag)
{
  int fd = MSVCRT_fdstart;

  TRACE(":handle (%d) allocating fd (%d)\n",hand,fd);
  if (fd >= MSVCRT_MAX_FILES)
  {
    WARN(":files exhausted!\n");
    return -1;
  }
  MSVCRT_handles[fd] = hand;
  MSVCRT_flags[fd] = flag;

  /* locate next free slot */
  if (fd == MSVCRT_fdend)
    MSVCRT_fdstart = ++MSVCRT_fdend;
  else
    while(MSVCRT_fdstart < MSVCRT_fdend &&
	  MSVCRT_handles[MSVCRT_fdstart] != INVALID_HANDLE_VALUE)
      MSVCRT_fdstart++;

  return fd;
}

/* INTERNAL: Allocate a FILE* for an fd slot
 * This is done lazily to avoid memory wastage for low level open/write
 * usage when a FILE* is not requested (but may be later).
 */
static MSVCRT_FILE* msvcrt_alloc_fp(int fd)
{
  TRACE(":fd (%d) allocating FILE*\n",fd);
  if (fd < 0 || fd >= MSVCRT_fdend ||
      MSVCRT_handles[fd] == INVALID_HANDLE_VALUE)
  {
    WARN(":invalid fd %d\n",fd);
    SET_THREAD_VAR(doserrno,0);
    SET_THREAD_VAR(errno,MSVCRT_EBADF);
    return NULL;
  }
  if (!MSVCRT_files[fd])
  {
    if ((MSVCRT_files[fd] = MSVCRT_calloc(sizeof(MSVCRT_FILE),1)))
    {
      MSVCRT_files[fd]->_file = fd;
      MSVCRT_files[fd]->_flag = MSVCRT_flags[fd];
      MSVCRT_files[fd]->_flag &= ~_IOAPPEND; /* mask out, see above */
    }
  }
  TRACE(":got FILE* (%p)\n",MSVCRT_files[fd]);
  return MSVCRT_files[fd];
}


/* INTERNAL: Set up stdin, stderr and stdout */
void msvcrt_init_io(void)
{
  int i;
  memset(MSVCRT__iob,0,3*sizeof(MSVCRT_FILE));
  MSVCRT_handles[0] = GetStdHandle(STD_INPUT_HANDLE);
  MSVCRT_flags[0] = MSVCRT__iob[0]._flag = _IOREAD;
  MSVCRT_handles[1] = GetStdHandle(STD_OUTPUT_HANDLE);
  MSVCRT_flags[1] = MSVCRT__iob[1]._flag = _IOWRT;
  MSVCRT_handles[2] = GetStdHandle(STD_ERROR_HANDLE);
  MSVCRT_flags[2] = MSVCRT__iob[2]._flag = _IOWRT;

  TRACE(":handles (%d)(%d)(%d)\n",MSVCRT_handles[0],
	MSVCRT_handles[1],MSVCRT_handles[2]);

  for (i = 0; i < 3; i++)
  {
    /* FILE structs for stdin/out/err are static and never deleted */
    MSVCRT_files[i] = &MSVCRT__iob[i];
    MSVCRT__iob[i]._file = i;
    MSVCRT_tempfiles[i] = NULL;
  }
}

/*********************************************************************
 *		__p__iob(MSVCRT.@)
 */
MSVCRT_FILE *__p__iob(void)
{
 return &MSVCRT__iob[0];
}

/*********************************************************************
 *		_access (MSVCRT.@)
 */
int _access(const char *filename, int mode)
{
  DWORD attr = GetFileAttributesA(filename);

  TRACE("(%s,%d) %ld\n",filename,mode,attr);

  if (!filename || attr == 0xffffffff)
  {
    MSVCRT__set_errno(GetLastError());
    return -1;
  }
  if ((attr & FILE_ATTRIBUTE_READONLY) && (mode & W_OK))
  {
    MSVCRT__set_errno(ERROR_ACCESS_DENIED);
    return -1;
  }
  return 0;
}

/*********************************************************************
 *		_waccess (MSVCRT.@)
 */
int _waccess(const WCHAR *filename, int mode)
{
  DWORD attr = GetFileAttributesW(filename);

  TRACE("(%s,%d) %ld\n",debugstr_w(filename),mode,attr);

  if (!filename || attr == 0xffffffff)
  {
    MSVCRT__set_errno(GetLastError());
    return -1;
  }
  if ((attr & FILE_ATTRIBUTE_READONLY) && (mode & W_OK))
  {
    MSVCRT__set_errno(ERROR_ACCESS_DENIED);
    return -1;
  }
  return 0;
}

/*********************************************************************
 *		_chmod (MSVCRT.@)
 */
int _chmod(const char *path, int flags)
{
  DWORD oldFlags = GetFileAttributesA(path);

  if (oldFlags != 0x0FFFFFFFF)
  {
    DWORD newFlags = (flags & _S_IWRITE)? oldFlags & ~FILE_ATTRIBUTE_READONLY:
      oldFlags | FILE_ATTRIBUTE_READONLY;

    if (newFlags == oldFlags || SetFileAttributesA(path, newFlags))
      return 0;
  }
  MSVCRT__set_errno(GetLastError());
  return -1;
}

/*********************************************************************
 *		_wchmod (MSVCRT.@)
 */
int _wchmod(const WCHAR *path, int flags)
{
  DWORD oldFlags = GetFileAttributesW(path);

  if (oldFlags != 0x0FFFFFFFF)
  {
    DWORD newFlags = (flags & _S_IWRITE)? oldFlags & ~FILE_ATTRIBUTE_READONLY:
      oldFlags | FILE_ATTRIBUTE_READONLY;

    if (newFlags == oldFlags || SetFileAttributesW(path, newFlags))
      return 0;
  }
  MSVCRT__set_errno(GetLastError());
  return -1;
}

/*********************************************************************
 *		_unlink (MSVCRT.@)
 */
int _unlink(const char *path)
{
  TRACE("(%s)\n",path);
  if(DeleteFileA(path))
    return 0;
  TRACE("failed (%ld)\n",GetLastError());
  MSVCRT__set_errno(GetLastError());
  return -1;
}

/*********************************************************************
 *		_wunlink (MSVCRT.@)
 */
int _wunlink(const WCHAR *path)
{
  TRACE("(%s)\n",debugstr_w(path));
  if(DeleteFileW(path))
    return 0;
  TRACE("failed (%ld)\n",GetLastError());
  MSVCRT__set_errno(GetLastError());
  return -1;
}

/*********************************************************************
 *		_close (MSVCRT.@)
 */
int _close(int fd)
{
  HANDLE hand = msvcrt_fdtoh(fd);

  TRACE(":fd (%d) handle (%d)\n",fd,hand);
  if (hand == INVALID_HANDLE_VALUE)
    return -1;

  /* Dont free std FILE*'s, they are not dynamic */
  if (fd > 2 && MSVCRT_files[fd])
    MSVCRT_free(MSVCRT_files[fd]);

  msvcrt_free_fd(fd);

  if (!CloseHandle(hand))
  {
    WARN(":failed-last error (%ld)\n",GetLastError());
    MSVCRT__set_errno(GetLastError());
    return -1;
  }
  if (MSVCRT_tempfiles[fd])
  {
    TRACE("deleting temporary file '%s'\n",MSVCRT_tempfiles[fd]);
    _unlink(MSVCRT_tempfiles[fd]);
    MSVCRT_free(MSVCRT_tempfiles[fd]);
    MSVCRT_tempfiles[fd] = NULL;
  }

  TRACE(":ok\n");
  return 0;
}

/*********************************************************************
 *		_commit (MSVCRT.@)
 */
int _commit(int fd)
{
  HANDLE hand = msvcrt_fdtoh(fd);

  TRACE(":fd (%d) handle (%d)\n",fd,hand);
  if (hand == INVALID_HANDLE_VALUE)
    return -1;

  if (!FlushFileBuffers(hand))
  {
    if (GetLastError() == ERROR_INVALID_HANDLE)
    {
      /* FlushFileBuffers fails for console handles
       * so we ignore this error.
       */
      return 0;
    }
    TRACE(":failed-last error (%ld)\n",GetLastError());
    MSVCRT__set_errno(GetLastError());
    return -1;
  }
  TRACE(":ok\n");
  return 0;
}

/*********************************************************************
 *		_eof (MSVCRT.@)
 */
int _eof(int fd)
{
  DWORD curpos,endpos;
  HANDLE hand = msvcrt_fdtoh(fd);

  TRACE(":fd (%d) handle (%d)\n",fd,hand);

  if (hand == INVALID_HANDLE_VALUE)
    return -1;

  /* If we have a FILE* for this file, the EOF flag
   * will be set by the read()/write() functions.
   */
  if (MSVCRT_files[fd])
    return MSVCRT_files[fd]->_flag & _IOEOF;

  /* Otherwise we do it the hard way */
  curpos = SetFilePointer(hand, 0, NULL, SEEK_CUR);
  endpos = SetFilePointer(hand, 0, NULL, FILE_END);

  if (curpos == endpos)
    return TRUE;

  SetFilePointer(hand, curpos, 0, FILE_BEGIN);
  return FALSE;
}

/*********************************************************************
 *		_fcloseall (MSVCRT.@)
 */
int _fcloseall(void)
{
  int num_closed = 0, i;

  for (i = 3; i < MSVCRT_fdend; i++)
    if (MSVCRT_handles[i] != INVALID_HANDLE_VALUE)
    {
      _close(i);
      num_closed++;
    }

  TRACE(":closed (%d) handles\n",num_closed);
  return num_closed;
}

/*********************************************************************
 *		_lseek (MSVCRT.@)
 */
LONG _lseek(int fd, LONG offset, int whence)
{
  DWORD ret;
  HANDLE hand = msvcrt_fdtoh(fd);

  TRACE(":fd (%d) handle (%d)\n",fd,hand);
  if (hand == INVALID_HANDLE_VALUE)
    return -1;

  if (whence < 0 || whence > 2)
  {
    SET_THREAD_VAR(errno,MSVCRT_EINVAL);
    return -1;
  }

  TRACE(":fd (%d) to 0x%08lx pos %s\n",
        fd,offset,(whence==SEEK_SET)?"SEEK_SET":
        (whence==SEEK_CUR)?"SEEK_CUR":
        (whence==SEEK_END)?"SEEK_END":"UNKNOWN");

  if ((ret = SetFilePointer(hand, offset, NULL, whence)) != 0xffffffff)
  {
    if (MSVCRT_files[fd])
      MSVCRT_files[fd]->_flag &= ~_IOEOF;
    /* FIXME: What if we seek _to_ EOF - is EOF set? */
    return ret;
  }
  TRACE(":error-last error (%ld)\n",GetLastError());
  if (MSVCRT_files[fd])
    switch(GetLastError())
    {
    case ERROR_NEGATIVE_SEEK:
    case ERROR_SEEK_ON_DEVICE:
      MSVCRT__set_errno(GetLastError());
      MSVCRT_files[fd]->_flag |= _IOERR;
      break;
    default:
      break;
    }
  return -1;
}

/*********************************************************************
 *		rewind (MSVCRT.@)
 */
void MSVCRT_rewind(MSVCRT_FILE* file)
{
  TRACE(":file (%p) fd (%d)\n",file,file->_file);
  _lseek(file->_file,0,SEEK_SET);
  file->_flag &= ~(_IOEOF | _IOERR);
}

/*********************************************************************
 *		_fdopen (MSVCRT.@)
 */
MSVCRT_FILE* _fdopen(int fd, const char *mode)
{
  MSVCRT_FILE* file = msvcrt_alloc_fp(fd);

  TRACE(":fd (%d) mode (%s) FILE* (%p)\n",fd,mode,file);
  if (file)
    MSVCRT_rewind(file);

  return file;
}

/*********************************************************************
 *		_wfdopen (MSVCRT.@)
 */
MSVCRT_FILE* _wfdopen(int fd, const WCHAR *mode)
{
  MSVCRT_FILE* file = msvcrt_alloc_fp(fd);

  TRACE(":fd (%d) mode (%s) FILE* (%p)\n",fd,debugstr_w(mode),file);
  if (file)
    MSVCRT_rewind(file);

  return file;
}

/*********************************************************************
 *		_filelength (MSVCRT.@)
 */
LONG _filelength(int fd)
{
  LONG curPos = _lseek(fd, 0, SEEK_CUR);
  if (curPos != -1)
  {
    LONG endPos = _lseek(fd, 0, SEEK_END);
    if (endPos != -1)
    {
      if (endPos != curPos)
        _lseek(fd, curPos, SEEK_SET);
      return endPos;
    }
  }
  return -1;
}

/*********************************************************************
 *		_fileno (MSVCRT.@)
 */
int _fileno(MSVCRT_FILE* file)
{
  TRACE(":FILE* (%p) fd (%d)\n",file,file->_file);
  return file->_file;
}

/*********************************************************************
 *		_flushall (MSVCRT.@)
 */
int _flushall(void)
{
  int num_flushed = 0, i = 3;

  while(i < MSVCRT_fdend)
    if (MSVCRT_handles[i] != INVALID_HANDLE_VALUE)
    {
      if (_commit(i) == -1)
	if (MSVCRT_files[i])
	  MSVCRT_files[i]->_flag |= _IOERR;
      num_flushed++;
    }

  TRACE(":flushed (%d) handles\n",num_flushed);
  return num_flushed;
}

/*********************************************************************
 *		_fstat (MSVCRT.@)
 */
int _fstat(int fd, struct _stat* buf)
{
  DWORD dw;
  BY_HANDLE_FILE_INFORMATION hfi;
  HANDLE hand = msvcrt_fdtoh(fd);

  TRACE(":fd (%d) stat (%p)\n",fd,buf);
  if (hand == INVALID_HANDLE_VALUE)
    return -1;

  if (!buf)
  {
    WARN(":failed-NULL buf\n");
    MSVCRT__set_errno(ERROR_INVALID_PARAMETER);
    return -1;
  }

  memset(&hfi, 0, sizeof(hfi));
  memset(buf, 0, sizeof(struct _stat));
  if (!GetFileInformationByHandle(hand, &hfi))
  {
    WARN(":failed-last error (%ld)\n",GetLastError());
    MSVCRT__set_errno(ERROR_INVALID_PARAMETER);
    return -1;
  }
  FIXME(":dwFileAttributes = %d, mode set to 0\n",hfi.dwFileAttributes);
  buf->st_nlink = hfi.nNumberOfLinks;
  buf->st_size  = hfi.nFileSizeLow;
  RtlTimeToSecondsSince1970(&hfi.ftLastAccessTime, &dw);
  buf->st_atime = dw;
  RtlTimeToSecondsSince1970(&hfi.ftLastWriteTime, &dw);
  buf->st_mtime = buf->st_ctime = dw;
  return 0;
}

/*********************************************************************
 *		_futime (MSVCRT.@)
 */
int _futime(int fd, struct _utimbuf *t)
{
  HANDLE hand = msvcrt_fdtoh(fd);
  FILETIME at, wt;

  if (!t)
  {
    MSVCRT_time_t currTime;
    MSVCRT_time(&currTime);
    RtlSecondsSince1970ToTime(currTime, &at);
    memcpy(&wt, &at, sizeof(wt));
  }
  else
  {
    RtlSecondsSince1970ToTime(t->actime, &at);
    if (t->actime == t->modtime)
      memcpy(&wt, &at, sizeof(wt));
    else
      RtlSecondsSince1970ToTime(t->modtime, &wt);
  }

  if (!SetFileTime(hand, NULL, &at, &wt))
  {
    MSVCRT__set_errno(GetLastError());
    return -1 ;
  }
  return 0;
}

/*********************************************************************
 *		_get_osfhandle (MSVCRT.@)
 */
long _get_osfhandle(int fd)
{
  HANDLE hand = msvcrt_fdtoh(fd);
  HANDLE newhand = hand;
  TRACE(":fd (%d) handle (%d)\n",fd,hand);

  if (hand != INVALID_HANDLE_VALUE)
  {
    /* FIXME: I'm not convinced that I should be copying the
     * handle here - it may be leaked if the app doesn't
     * close it (and the API docs dont say that it should)
     * Not duplicating it means that it can't be inherited
     * and so lcc's wedit doesn't cope when it passes it to
     * child processes. I've an idea that it should either
     * be copied by CreateProcess, or marked as inheritable
     * when initialised, or maybe both? JG 21-9-00.
     */
    DuplicateHandle(GetCurrentProcess(),hand,GetCurrentProcess(),
		    &newhand,0,TRUE,DUPLICATE_SAME_ACCESS);
  }
  return newhand;
}

/*********************************************************************
 *		_isatty (MSVCRT.@)
 */
int _isatty(int fd)
{
  HANDLE hand = msvcrt_fdtoh(fd);

  TRACE(":fd (%d) handle (%d)\n",fd,hand);
  if (hand == INVALID_HANDLE_VALUE)
    return 0;

  return GetFileType(fd) == FILE_TYPE_CHAR? 1 : 0;
}

/*********************************************************************
 *		_mktemp (MSVCRT.@)
 */
char *_mktemp(char *pattern)
{
  int numX = 0;
  char *retVal = pattern;
  int id;
  char letter = 'a';

  while(*pattern)
    numX = (*pattern++ == 'X')? numX + 1 : 0;
  if (numX < 5)
    return NULL;
  pattern--;
  id = GetCurrentProcessId();
  numX = 6;
  while(numX--)
  {
    int tempNum = id / 10;
    *pattern-- = id - (tempNum * 10) + '0';
    id = tempNum;
  }
  pattern++;
  do
  {
    if (GetFileAttributesA(retVal) == 0xFFFFFFFF &&
        GetLastError() == ERROR_FILE_NOT_FOUND)
      return retVal;
    *pattern = letter++;
  } while(letter != '|');
  return NULL;
}

/*********************************************************************
 *		_wmktemp (MSVCRT.@)
 */
WCHAR *_wmktemp(WCHAR *pattern)
{
  int numX = 0;
  WCHAR *retVal = pattern;
  int id;
  WCHAR letter = (WCHAR)L'a';

  while(*pattern)
    numX = (*pattern++ == (WCHAR)L'X')? numX + 1 : 0;
  if (numX < 5)
    return NULL;
  pattern--;
  id = GetCurrentProcessId();
  numX = 6;
  while(numX--)
  {
    int tempNum = id / 10;
    *pattern-- = id - (tempNum * 10) + (WCHAR)L'0';
    id = tempNum;
  }
  pattern++;
  do
  {
    if (GetFileAttributesW(retVal) == 0xFFFFFFFF &&
        GetLastError() == ERROR_FILE_NOT_FOUND)
      return retVal;
    *pattern = letter++;
  } while(letter != (WCHAR)L'|');
  return NULL;
}

/*********************************************************************
 *		_open (MSVCRT.@)
 */
int _open(const char *path,int flags,...)
{
  DWORD access = 0, creation = 0;
  int ioflag = 0, fd;
  HANDLE hand;
  SECURITY_ATTRIBUTES sa;
  
  TRACE(":file (%s) mode 0x%04x\n",path,flags);

  switch(flags & (_O_RDONLY | _O_WRONLY | _O_RDWR))
  {
  case _O_RDONLY:
    access |= GENERIC_READ;
    ioflag |= _IOREAD;
    break;
  case _O_WRONLY:
    access |= GENERIC_WRITE;
    ioflag |= _IOWRT;
    break;
  case _O_RDWR:
    access |= GENERIC_WRITE | GENERIC_READ;
    ioflag |= _IORW;
    break;
  }

  if (flags & _O_CREAT)
  {
    if (flags & _O_EXCL)
      creation = CREATE_NEW;
    else if (flags & _O_TRUNC)
      creation = CREATE_ALWAYS;
    else
      creation = OPEN_ALWAYS;
  }
  else  /* no _O_CREAT */
  {
    if (flags & _O_TRUNC)
      creation = TRUNCATE_EXISTING;
    else
      creation = OPEN_EXISTING;
  }
  if (flags & _O_APPEND)
    ioflag |= _IOAPPEND;


  flags |= _O_BINARY; /* FIXME: Default to text */

  if (flags & _O_TEXT)
  {
    /* Dont warn when writing */
    if (ioflag & GENERIC_READ)
      FIXME(":TEXT node not implemented\n");
    flags &= ~_O_TEXT;
  }

  if (flags & ~(_O_BINARY|_O_TEXT|_O_APPEND|_O_TRUNC|_O_EXCL
                |_O_CREAT|_O_RDWR|_O_TEMPORARY))
    TRACE(":unsupported flags 0x%04x\n",flags);
      
  sa.nLength              = sizeof( SECURITY_ATTRIBUTES );
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle       = TRUE;

  hand = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, creation, FILE_ATTRIBUTE_NORMAL, 0);

  if (hand == INVALID_HANDLE_VALUE)
  {
    WARN(":failed-last error (%ld)\n",GetLastError());
    MSVCRT__set_errno(GetLastError());
    return -1;
  }

  fd = msvcrt_alloc_fd(hand, ioflag);

  TRACE(":fd (%d) handle (%d)\n",fd, hand);

  if (fd > 0)
  {
    if (flags & _O_TEMPORARY)
      MSVCRT_tempfiles[fd] = _strdup(path);
    if (ioflag & _IOAPPEND)
      _lseek(fd, 0, FILE_END);
  }

  return fd;
}

/*********************************************************************
 *		_wopen (MSVCRT.@)
 */
int _wopen(const WCHAR *path,int flags,...)
{
  const unsigned int len = strlenW(path);
  char *patha = MSVCRT_calloc(len + 1,1);
  if (patha && WideCharToMultiByte(CP_ACP,0,path,len,patha,len,NULL,NULL))
  {
    int retval = _open(patha,flags);
    MSVCRT_free(patha);
    return retval;
  }

  MSVCRT__set_errno(GetLastError());
  return -1;
}

/*********************************************************************
 *		_sopen (MSVCRT.@)
 */
int MSVCRT__sopen(const char *path,int oflags,int shflags)
{
  return _open(path, oflags | shflags);
}

/*********************************************************************
 *		_creat (MSVCRT.@)
 */
int _creat(const char *path, int flags)
{
  int usedFlags = (flags & _O_TEXT)| _O_CREAT| _O_WRONLY| _O_TRUNC;
  return _open(path, usedFlags);
}

/*********************************************************************
 *		_wcreat (MSVCRT.@)
 */
int _wcreat(const WCHAR *path, int flags)
{
  int usedFlags = (flags & _O_TEXT)| _O_CREAT| _O_WRONLY| _O_TRUNC;
  return _wopen(path, usedFlags);
}

/*********************************************************************
 *		_open_osfhandle (MSVCRT.@)
 */
int _open_osfhandle(long hand, int flags)
{
  int fd = msvcrt_alloc_fd(hand,flags);
  TRACE(":handle (%ld) fd (%d)\n",hand,fd);
  return fd;
}

/*********************************************************************
 *		_rmtmp (MSVCRT.@)
 */
int _rmtmp(void)
{
  int num_removed = 0, i;

  for (i = 3; i < MSVCRT_fdend; i++)
    if (MSVCRT_tempfiles[i])
    {
      _close(i);
      num_removed++;
    }

  if (num_removed)
    TRACE(":removed (%d) temp files\n",num_removed);
  return num_removed;
}

/*********************************************************************
 *		_read (MSVCRT.@)
 */
int _read(int fd, void *buf, unsigned int count)
{
  DWORD num_read;
  HANDLE hand = msvcrt_fdtoh(fd);

  /* Dont trace small reads, it gets *very* annoying */
  if (count > 4)
    TRACE(":fd (%d) handle (%d) buf (%p) len (%d)\n",fd,hand,buf,count);
  if (hand == INVALID_HANDLE_VALUE)
    return -1;

  /* Set _cnt to 0 so optimised binaries will call our implementation
   * of putc/getc. See _filbuf/_flsbuf comments.
   */
  if (MSVCRT_files[fd])
    MSVCRT_files[fd]->_cnt = 0;

  if (ReadFile(hand, buf, count, &num_read, NULL))
  {
    if (num_read != count && MSVCRT_files[fd])
    {
      TRACE(":EOF\n");
      MSVCRT_files[fd]->_flag |= _IOEOF;
    }
    return num_read;
  }
  TRACE(":failed-last error (%ld)\n",GetLastError());
  if (MSVCRT_files[fd])
     MSVCRT_files[fd]->_flag |= _IOERR;
  return -1;
}

/*********************************************************************
 *		_getw (MSVCRT.@)
 */
int _getw(MSVCRT_FILE* file)
{
  int i;
  if (_read(file->_file, &i, sizeof(int)) != 1)
    return MSVCRT_EOF;
  return i;
}

/*********************************************************************
 *		_setmode (MSVCRT.@)
 */
int _setmode(int fd,int mode)
{
  if (mode & _O_TEXT)
    FIXME("fd (%d) mode (%d) TEXT not implemented\n",fd,mode);
  return 0;
}

/*********************************************************************
 *		_stat (MSVCRT.@)
 */
int _stat(const char* path, struct _stat * buf)
{
  DWORD dw;
  WIN32_FILE_ATTRIBUTE_DATA hfi;
  unsigned short mode = MSVCRT_S_IREAD;
  int plen;

  TRACE(":file (%s) buf(%p)\n",path,buf);

  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &hfi))
  {
      TRACE("failed (%ld)\n",GetLastError());
      MSVCRT__set_errno(ERROR_FILE_NOT_FOUND);
      return -1;
  }

  memset(buf,0,sizeof(struct _stat));

  /* FIXME: rdev isnt drive num,despite what the docs say-what is it? */
  if (isalpha(*path))
    buf->st_dev = buf->st_rdev = toupper(*path - 'A'); /* drive num */
  else
    buf->st_dev = buf->st_rdev = _getdrive() - 1;

  plen = strlen(path);

  /* Dir, or regular file? */
  if ((hfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (path[plen-1] == '\\'))
    mode |= (_S_IFDIR | MSVCRT_S_IEXEC);
  else
  {
    mode |= _S_IFREG;
    /* executable? */
    if (plen > 6 && path[plen-4] == '.')  /* shortest exe: "\x.exe" */
    {
      unsigned int ext = tolower(path[plen-1]) | (tolower(path[plen-2]) << 8) |
                                 (tolower(path[plen-3]) << 16);
      if (ext == EXE || ext == BAT || ext == CMD || ext == COM)
          mode |= MSVCRT_S_IEXEC;
    }
  }

  if (!(hfi.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
    mode |= MSVCRT_S_IWRITE;

  buf->st_mode  = mode;
  buf->st_nlink = 1;
  buf->st_size  = hfi.nFileSizeLow;
  RtlTimeToSecondsSince1970(&hfi.ftLastAccessTime, &dw);
  buf->st_atime = dw;
  RtlTimeToSecondsSince1970(&hfi.ftLastWriteTime, &dw);
  buf->st_mtime = buf->st_ctime = dw;
  TRACE("\n%d %d %d %ld %ld %ld\n", buf->st_mode,buf->st_nlink,buf->st_size,
    buf->st_atime,buf->st_mtime, buf->st_ctime);
  return 0;
}

/*********************************************************************
 *		_wstat (MSVCRT.@)
 */
int _wstat(const WCHAR* path, struct _stat * buf)
{
  DWORD dw;
  WIN32_FILE_ATTRIBUTE_DATA hfi;
  unsigned short mode = MSVCRT_S_IREAD;
  int plen;

  TRACE(":file (%s) buf(%p)\n",debugstr_w(path),buf);

  if (!GetFileAttributesExW(path, GetFileExInfoStandard, &hfi))
  {
      TRACE("failed (%ld)\n",GetLastError());
      MSVCRT__set_errno(ERROR_FILE_NOT_FOUND);
      return -1;
  }

  memset(buf,0,sizeof(struct _stat));

  /* FIXME: rdev isn't drive num, despite what the docs says-what is it? */
  if (MSVCRT_iswalpha(*path))
    buf->st_dev = buf->st_rdev = toupperW(*path - (WCHAR)L'A'); /* drive num */
  else
    buf->st_dev = buf->st_rdev = _getdrive() - 1;

  plen = strlenW(path);

  /* Dir, or regular file? */
  if ((hfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (path[plen-1] == (WCHAR)L'\\'))
    mode |= (_S_IFDIR | MSVCRT_S_IEXEC);
  else
  {
    mode |= _S_IFREG;
    /* executable? */
    if (plen > 6 && path[plen-4] == (WCHAR)L'.')  /* shortest exe: "\x.exe" */
    {
      ULONGLONG ext = tolowerW(path[plen-1]) | (tolowerW(path[plen-2]) << 16) |
                               ((ULONGLONG)tolowerW(path[plen-3]) << 32);
      if (ext == WCEXE || ext == WCBAT || ext == WCCMD || ext == WCCOM)
        mode |= MSVCRT_S_IEXEC;
    }
  }

  if (!(hfi.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
    mode |= MSVCRT_S_IWRITE;

  buf->st_mode  = mode;
  buf->st_nlink = 1;
  buf->st_size  = hfi.nFileSizeLow;
  RtlTimeToSecondsSince1970(&hfi.ftLastAccessTime, &dw);
  buf->st_atime = dw;
  RtlTimeToSecondsSince1970(&hfi.ftLastWriteTime, &dw);
  buf->st_mtime = buf->st_ctime = dw;
  TRACE("\n%d %d %d %ld %ld %ld\n", buf->st_mode,buf->st_nlink,buf->st_size,
        buf->st_atime,buf->st_mtime, buf->st_ctime);
  return 0;
}

/*********************************************************************
 *		_tell (MSVCRT.@)
 */
LONG _tell(int fd)
{
  return _lseek(fd, 0, SEEK_CUR);
}

/*********************************************************************
 *		_tempnam (MSVCRT.@)
 */
char *_tempnam(const char *dir, const char *prefix)
{
  char tmpbuf[MAX_PATH];

  TRACE("dir (%s) prefix (%s)\n",dir,prefix);
  if (GetTempFileNameA(dir,prefix,0,tmpbuf))
  {
    TRACE("got name (%s)\n",tmpbuf);
    return _strdup(tmpbuf);
  }
  TRACE("failed (%ld)\n",GetLastError());
  return NULL;
}

/*********************************************************************
 *		_wtempnam (MSVCRT.@)
 */
WCHAR *_wtempnam(const WCHAR *dir, const WCHAR *prefix)
{
  WCHAR tmpbuf[MAX_PATH];

  TRACE("dir (%s) prefix (%s)\n",debugstr_w(dir),debugstr_w(prefix));
  if (GetTempFileNameW(dir,prefix,0,tmpbuf))
  {
    TRACE("got name (%s)\n",debugstr_w(tmpbuf));
    return _wcsdup(tmpbuf);
  }
  TRACE("failed (%ld)\n",GetLastError());
  return NULL;
}

/*********************************************************************
 *		_umask (MSVCRT.@)
 */
int _umask(int umask)
{
  int old_umask = MSVCRT_umask;
  TRACE("(%d)\n",umask);
  MSVCRT_umask = umask;
  return old_umask;
}

/*********************************************************************
 *		_utime (MSVCRT.@)
 */
int _utime(const char* path, struct _utimbuf *t)
{
  int fd = _open(path, _O_WRONLY | _O_BINARY);

  if (fd > 0)
  {
    int retVal = _futime(fd, t);
    _close(fd);
    return retVal;
  }
  return -1;
}

/*********************************************************************
 *		_wutime (MSVCRT.@)
 */
int _wutime(const WCHAR* path, struct _utimbuf *t)
{
  int fd = _wopen(path, _O_WRONLY | _O_BINARY);

  if (fd > 0)
  {
    int retVal = _futime(fd, t);
    _close(fd);
    return retVal;
  }
  return -1;
}

/*********************************************************************
 *		_write (MSVCRT.@)
 */
int _write(int fd, const void* buf, unsigned int count)
{
  DWORD num_written;
  HANDLE hand = msvcrt_fdtoh(fd);

  /* Dont trace small writes, it gets *very* annoying */
//  if (count > 32)
//    TRACE(":fd (%d) handle (%d) buf (%p) len (%d)\n",fd,hand,buf,count);
  if (hand == INVALID_HANDLE_VALUE)
    return -1;

  /* If appending, go to EOF */
  if (MSVCRT_flags[fd] & _IOAPPEND)
    _lseek(fd, 0, FILE_END);

  /* Set _cnt to 0 so optimised binaries will call our implementation
   * of putc/getc.
   */
  if (MSVCRT_files[fd])
    MSVCRT_files[fd]->_cnt = 0;

  if (WriteFile(hand, buf, count, &num_written, NULL)
      &&  (num_written == count))
    return num_written;

  TRACE(":failed-last error (%ld)\n",GetLastError());
  if (MSVCRT_files[fd])
     MSVCRT_files[fd]->_flag |= _IOERR;

  return -1;
}

/*********************************************************************
 *		_putw (MSVCRT.@)
 */
int _putw(int val, MSVCRT_FILE* file)
{
  return _write(file->_file, &val, sizeof(val)) == 1? val : MSVCRT_EOF;
}

/*********************************************************************
 *		clearerr (MSVCRT.@)
 */
void MSVCRT_clearerr(MSVCRT_FILE* file)
{
  TRACE(":file (%p) fd (%d)\n",file,file->_file);
  file->_flag &= ~(_IOERR | _IOEOF);
}

/*********************************************************************
 *		fclose (MSVCRT.@)
 */
int MSVCRT_fclose(MSVCRT_FILE* file)
{
  int r;
  r=_close(file->_file);
  return ((r==MSVCRT_EOF) || (file->_flag & _IOERR) ? MSVCRT_EOF : 0);
}

/*********************************************************************
 *		feof (MSVCRT.@)
 */
int MSVCRT_feof(MSVCRT_FILE* file)
{
  return file->_flag & _IOEOF;
}

/*********************************************************************
 *		ferror (MSVCRT.@)
 */
int MSVCRT_ferror(MSVCRT_FILE* file)
{
  return file->_flag & _IOERR;
}

/*********************************************************************
 *		fflush (MSVCRT.@)
 */
int MSVCRT_fflush(MSVCRT_FILE* file)
{
  return _commit(file->_file);
}

/*********************************************************************
 *		fgetc (MSVCRT.@)
 */
int MSVCRT_fgetc(MSVCRT_FILE* file)
{
  char c;
  if (_read(file->_file,&c,1) != 1)
    return MSVCRT_EOF;
  return c;
}

/*********************************************************************
 *		_fgetchar (MSVCRT.@)
 */
int _fgetchar(void)
{
  return MSVCRT_fgetc(MSVCRT_stdin);
}

/*********************************************************************
 *		_filbuf (MSVCRT.@)
 */
int _filbuf(MSVCRT_FILE* file)
{
  return MSVCRT_fgetc(file);
}

/*********************************************************************
 *		fgetpos (MSVCRT.@)
 */
int MSVCRT_fgetpos(MSVCRT_FILE* file, MSVCRT_fpos_t *pos)
{
  *pos = _tell(file->_file);
  return (*pos == -1? -1 : 0);
}

/*********************************************************************
 *		fgets (MSVCRT.@)
 */
char *MSVCRT_fgets(char *s, int size, MSVCRT_FILE* file)
{
  int    cc;
  char * buf_start = s;

  TRACE(":file(%p) fd (%d) str (%p) len (%d)\n",
	file,file->_file,s,size);

  /* BAD, for the whole WINE process blocks... just done this way to test
   * windows95's ftp.exe.
   * JG - Is this true now we use ReadFile() on stdin too?
   */
  for(cc = MSVCRT_fgetc(file); cc != MSVCRT_EOF && cc != '\n';
      cc = MSVCRT_fgetc(file))
    if (cc != '\r')
    {
      if (--size <= 0) break;
      *s++ = (char)cc;
    }
  if ((cc == MSVCRT_EOF) && (s == buf_start)) /* If nothing read, return 0*/
  {
    TRACE(":nothing read\n");
    return 0;
  }
  if (cc == '\n')
    if (--size > 0)
      *s++ = '\n';
  *s = '\0';
  TRACE(":got '%s'\n", buf_start);
  return buf_start;
}

/*********************************************************************
 *		fgetwc (MSVCRT.@)
 */
MSVCRT_wint_t MSVCRT_fgetwc(MSVCRT_FILE* file)
{
  MSVCRT_wint_t wc;
  if (_read(file->_file, &wc, sizeof(wc)) != sizeof(wc))
    return MSVCRT_WEOF;
  return wc;
}

/*********************************************************************
 *		getwc (MSVCRT.@)
 */
MSVCRT_wint_t MSVCRT_getwc(MSVCRT_FILE* file)
{
  return MSVCRT_fgetwc(file);
}

/*********************************************************************
 *		_fgetwchar (MSVCRT.@)
 */
MSVCRT_wint_t _fgetwchar(void)
{
  return MSVCRT_fgetwc(MSVCRT_stdin);
}

/*********************************************************************
 *		getwchar (MSVCRT.@)
 */
MSVCRT_wint_t MSVCRT_getwchar(void)
{
  return _fgetwchar();
}

/*********************************************************************
 *		fputwc (MSVCRT.@)
 */
MSVCRT_wint_t MSVCRT_fputwc(MSVCRT_wint_t wc, MSVCRT_FILE* file)
{
  if (_write(file->_file, &wc, sizeof(wc)) != sizeof(wc))
    return MSVCRT_WEOF;
  return wc;
}

/*********************************************************************
 *		_fputwchar (MSVCRT.@)
 */
MSVCRT_wint_t _fputwchar(MSVCRT_wint_t wc)
{
  return MSVCRT_fputwc(wc, MSVCRT_stdout);
}

/*********************************************************************
 *		fopen (MSVCRT.@)
 */
MSVCRT_FILE* MSVCRT_fopen(const char *path, const char *mode)
{
  MSVCRT_FILE* file;
  int flags = 0, plus = 0, fd;
  const char* search = mode;

  TRACE("(%s,%s)\n",path,mode);

  while (*search)
    if (*search++ == '+')
      plus = 1;

  /* map mode string to open() flags. "man fopen" for possibilities. */
  switch(*mode++)
  {
  case 'R': case 'r':
    flags = (plus ? _O_RDWR : _O_RDONLY);
    break;
  case 'W': case 'w':
    flags = _O_CREAT | _O_TRUNC | (plus  ? _O_RDWR : _O_WRONLY);
    break;
  case 'A': case 'a':
    flags = _O_CREAT | _O_APPEND | (plus  ? _O_RDWR : _O_WRONLY);
    break;
  default:
    return NULL;
  }

  while (*mode)
    switch (*mode++)
    {
    case 'B': case 'b':
      flags |=  _O_BINARY;
      flags &= ~_O_TEXT;
      break;
    case 'T': case 't':
      flags |=  _O_TEXT;
      flags &= ~_O_BINARY;
      break;
    case '+':
      break;
    default:
      FIXME(":unknown flag %c not supported\n",mode[-1]);
    }

  fd = _open(path, flags);

  if (fd < 0)
    return NULL;

  file = msvcrt_alloc_fp(fd);
  TRACE(":got (%p)\n",file);
  if (!file)
    _close(fd);

  return file;
}

/*********************************************************************
 *		_wfopen (MSVCRT.@)
 */
MSVCRT_FILE *_wfopen(const WCHAR *path, const WCHAR *mode)
{
  const unsigned int plen = strlenW(path), mlen = strlenW(mode);
  char *patha = MSVCRT_calloc(plen + 1, 1);
  char *modea = MSVCRT_calloc(mlen + 1, 1);

  TRACE("(%s,%s)\n",debugstr_w(path),debugstr_w(mode));

  if (patha && modea &&
      WideCharToMultiByte(CP_ACP,0,path,plen,patha,plen,NULL,NULL) &&
      WideCharToMultiByte(CP_ACP,0,mode,mlen,modea,mlen,NULL,NULL))
  {
    MSVCRT_FILE *retval = MSVCRT_fopen(patha,modea);
    MSVCRT_free(patha);
    MSVCRT_free(modea);
    return retval;
  }

  MSVCRT__set_errno(GetLastError());
  return NULL;
}

/*********************************************************************
 *		_fsopen (MSVCRT.@)
 */
MSVCRT_FILE*  _fsopen(const char *path, const char *mode, int share)
{
  FIXME(":(%s,%s,%d),ignoring share mode!\n",path,mode,share);
  return MSVCRT_fopen(path,mode);
}

/*********************************************************************
 *		_wfsopen (MSVCRT.@)
 */
MSVCRT_FILE*  _wfsopen(const WCHAR *path, const WCHAR *mode, int share)
{
  FIXME(":(%s,%s,%d),ignoring share mode!\n",
        debugstr_w(path),debugstr_w(mode),share);
  return _wfopen(path,mode);
}

/*********************************************************************
 *		fputc (MSVCRT.@)
 */
int MSVCRT_fputc(int c, MSVCRT_FILE* file)
{
  return _write(file->_file, &c, 1) == 1? c : MSVCRT_EOF;
}

/*********************************************************************
 *		_flsbuf (MSVCRT.@)
 */
int _flsbuf(int c, MSVCRT_FILE* file)
{
  return MSVCRT_fputc(c,file);
}

/*********************************************************************
 *		_fputchar (MSVCRT.@)
 */
int _fputchar(int c)
{
  return MSVCRT_fputc(c, MSVCRT_stdout);
}

/*********************************************************************
 *		fread (MSVCRT.@)
 */
MSVCRT_size_t MSVCRT_fread(void *ptr, MSVCRT_size_t size, MSVCRT_size_t nmemb, MSVCRT_FILE* file)
{
  int read = _read(file->_file,ptr, size * nmemb);
  if (read <= 0)
    return 0;
  return read / size;
}

/*********************************************************************
 *		freopen (MSVCRT.@)
 *
 */
MSVCRT_FILE* MSVCRT_freopen(const char *path, const char *mode,MSVCRT_FILE* file)
{
  MSVCRT_FILE* newfile;
  int fd;

  TRACE(":path (%p) mode (%s) file (%p) fd (%d)\n",path,mode,file,file->_file);
  if (!file || ((fd = file->_file) < 0) || fd > MSVCRT_fdend)
    return NULL;

  if (fd > 2)
  {
    FIXME(":reopen on user file not implemented!\n");
    MSVCRT__set_errno(ERROR_CALL_NOT_IMPLEMENTED);
    return NULL;
  }

  /* first, create the new file */
  if ((newfile = MSVCRT_fopen(path,mode)) == NULL)
    return NULL;

  if (fd < 3 && SetStdHandle(fd == 0 ? STD_INPUT_HANDLE :
     (fd == 1? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE),
      MSVCRT_handles[newfile->_file]))
  {
    /* Redirecting std handle to file , copy over.. */
    MSVCRT_handles[fd] = MSVCRT_handles[newfile->_file];
    MSVCRT_flags[fd] = MSVCRT_flags[newfile->_file];
    memcpy(&MSVCRT__iob[fd], newfile, sizeof (MSVCRT_FILE));
    MSVCRT__iob[fd]._file = fd;
    /* And free up the resources allocated by fopen, but
     * not the HANDLE we copied. */
    MSVCRT_free(MSVCRT_files[fd]);
    msvcrt_free_fd(newfile->_file);
    return &MSVCRT__iob[fd];
  }

  WARN(":failed-last error (%ld)\n",GetLastError());
  MSVCRT_fclose(newfile);
  MSVCRT__set_errno(GetLastError());
  return NULL;
}

/*********************************************************************
 *		fsetpos (MSVCRT.@)
 */
int MSVCRT_fsetpos(MSVCRT_FILE* file, MSVCRT_fpos_t *pos)
{
  return _lseek(file->_file,*pos,SEEK_SET);
}

/*********************************************************************
 *		fscanf (MSVCRT.@)
 */
int MSVCRT_fscanf(MSVCRT_FILE* file, const char *format, ...)
{
    /* NOTE: If you extend this function, extend MSVCRT__cscanf in console.c too */
    int rd = 0;
    int nch;
    va_list ap;
    if (!*format) return 0;
    WARN("%p (\"%s\"): semi-stub\n", file, format);
    nch = MSVCRT_fgetc(file);
    va_start(ap, format);
    while (*format) {
        if (*format == ' ') {
            /* skip whitespace */
            while ((nch!=MSVCRT_EOF) && isspace(nch))
                nch = MSVCRT_fgetc(file);
        }
        else if (*format == '%') {
            int st = 0;
            format++;
            switch(*format) {
            case 'd': { /* read an integer */
                    int*val = va_arg(ap, int*);
                    int cur = 0;
                    /* skip initial whitespace */
                    while ((nch!=MSVCRT_EOF) && isspace(nch))
                        nch = MSVCRT_fgetc(file);
                    /* get sign and first digit */
                    if (nch == '-') {
                        nch = MSVCRT_fgetc(file);
                        if (isdigit(nch))
                            cur = -(nch - '0');
                        else break;
                    } else {
                        if (isdigit(nch))
                            cur = nch - '0';
                        else break;
                    }
                    nch = MSVCRT_fgetc(file);
                    /* read until no more digits */
                    while ((nch!=MSVCRT_EOF) && isdigit(nch)) {
                        cur = cur*10 + (nch - '0');
                        nch = MSVCRT_fgetc(file);
                    }
                    st = 1;
                    *val = cur;
                }
                break;
            case 'f': { /* read a float */
                    float*val = va_arg(ap, float*);
                    float cur = 0;
                    /* skip initial whitespace */
                    while ((nch!=MSVCRT_EOF) && isspace(nch))
                        nch = MSVCRT_fgetc(file);
                    /* get sign and first digit */
                    if (nch == '-') {
                        nch = MSVCRT_fgetc(file);
                        if (isdigit(nch))
                            cur = -(nch - '0');
                        else break;
                    } else {
                        if (isdigit(nch))
                            cur = nch - '0';
                        else break;
                    }
                    /* read until no more digits */
                    while ((nch!=MSVCRT_EOF) && isdigit(nch)) {
                        cur = cur*10 + (nch - '0');
                        nch = MSVCRT_fgetc(file);
                    }
                    if (nch == '.') {
                        /* handle decimals */
                        float dec = 1;
                        nch = MSVCRT_fgetc(file);
                        while ((nch!=MSVCRT_EOF) && isdigit(nch)) {
                            dec /= 10;
                            cur += dec * (nch - '0');
                            nch = MSVCRT_fgetc(file);
                        }
                    }
                    st = 1;
                    *val = cur;
                }
                break;
            case 's': { /* read a word */
                    char*str = va_arg(ap, char*);
                    char*sptr = str;
                    /* skip initial whitespace */
                    while ((nch!=MSVCRT_EOF) && isspace(nch))
                        nch = MSVCRT_fgetc(file);
                    /* read until whitespace */
                    while ((nch!=MSVCRT_EOF) && !isspace(nch)) {
                        *sptr++ = nch; st++;
                        nch = MSVCRT_fgetc(file);
                    }
                    /* terminate */
                    *sptr = 0;
                    TRACE("read word: %s\n", str);
                }
                break;
            default: FIXME("unhandled: %%%c\n", *format);
            }
            if (st) rd++;
            else break;
        }
        else {
            /* check for character match */
            if (nch == *format)
               nch = MSVCRT_fgetc(file);
            else break;
        }
        format++;
    }
    va_end(ap);
    if (nch!=MSVCRT_EOF) {
        WARN("need ungetch\n");
    }
    TRACE("returning %d\n", rd);
    return rd;
}

/*********************************************************************
 *		fseek (MSVCRT.@)
 */
int MSVCRT_fseek(MSVCRT_FILE* file, long offset, int whence)
{
  return _lseek(file->_file,offset,whence);
}

/*********************************************************************
 *		ftell (MSVCRT.@)
 */
LONG MSVCRT_ftell(MSVCRT_FILE* file)
{
  return _tell(file->_file);
}

/*********************************************************************
 *		fwrite (MSVCRT.@)
 */
MSVCRT_size_t MSVCRT_fwrite(const void *ptr, MSVCRT_size_t size, MSVCRT_size_t nmemb, MSVCRT_FILE* file)
{
  int written = _write(file->_file, ptr, size * nmemb);
  if (written <= 0)
    return 0;
  return written / size;
}

/*********************************************************************
 *		fputs (MSVCRT.@)
 */
int MSVCRT_fputs(const char *s, MSVCRT_FILE* file)
{
  return MSVCRT_fwrite(s,strlen(s),1,file) == 1 ? 0 : MSVCRT_EOF;
}

/*********************************************************************
 *		fputws (MSVCRT.@)
 */
int MSVCRT_fputws(const WCHAR *s, MSVCRT_FILE* file)
{
  return MSVCRT_fwrite(s,strlenW(s),1,file) == 1 ? 0 : MSVCRT_EOF;
}

/*********************************************************************
 *		getchar (MSVCRT.@)
 */
int MSVCRT_getchar(void)
{
  return MSVCRT_fgetc(MSVCRT_stdin);
}

/*********************************************************************
 *		getc (MSVCRT.@)
 */
int MSVCRT_getc(MSVCRT_FILE* file)
{
  return MSVCRT_fgetc(file);
}

/*********************************************************************
 *		gets (MSVCRT.@)
 */
char *MSVCRT_gets(char *buf)
{
  int    cc;
  char * buf_start = buf;

  /* BAD, for the whole WINE process blocks... just done this way to test
   * windows95's ftp.exe.
   * JG 19/9/00: Is this still true, now we are using ReadFile?
   */
  for(cc = MSVCRT_fgetc(MSVCRT_stdin); cc != MSVCRT_EOF && cc != '\n';
      cc = MSVCRT_fgetc(MSVCRT_stdin))
  if(cc != '\r') *buf++ = (char)cc;

  *buf = '\0';

  TRACE("got '%s'\n", buf_start);
  return buf_start;
}

/*********************************************************************
 *		putc (MSVCRT.@)
 */
int MSVCRT_putc(int c, MSVCRT_FILE* file)
{
  return MSVCRT_fputc(c, file);
}

/*********************************************************************
 *		putchar (MSVCRT.@)
 */
int MSVCRT_putchar(int c)
{
  return MSVCRT_fputc(c, MSVCRT_stdout);
}

/*********************************************************************
 *		puts (MSVCRT.@)
 */
int MSVCRT_puts(const char *s)
{
  int retval = MSVCRT_EOF;
  if (MSVCRT_fwrite(s,strlen(s),1,MSVCRT_stdout) == 1)
    return MSVCRT_fwrite("\n",1,1,MSVCRT_stdout) == 1 ? 0 : MSVCRT_EOF;
  return retval;
}

/*********************************************************************
 *		_putws (MSVCRT.@)
 */
int _putws(const WCHAR *s)
{
  static const WCHAR nl = (WCHAR)L'\n';
  if (MSVCRT_fwrite(s,strlenW(s),1,MSVCRT_stdout) == 1)
    return MSVCRT_fwrite(&nl,sizeof(nl),1,MSVCRT_stdout) == 1 ? 0 : MSVCRT_EOF;
  return MSVCRT_EOF;
}

/*********************************************************************
 *		remove (MSVCRT.@)
 */
int MSVCRT_remove(const char *path)
{
  TRACE("(%s)\n",path);
  if (DeleteFileA(path))
    return 0;
  TRACE(":failed (%ld)\n",GetLastError());
  MSVCRT__set_errno(GetLastError());
  return -1;
}

/*********************************************************************
 *		_wremove (MSVCRT.@)
 */
int _wremove(const WCHAR *path)
{
  TRACE("(%s)\n",debugstr_w(path));
  if (DeleteFileW(path))
    return 0;
  TRACE(":failed (%ld)\n",GetLastError());
  MSVCRT__set_errno(GetLastError());
  return -1;
}

/*********************************************************************
 *		scanf (MSVCRT.@)
 */
int MSVCRT_scanf(const char *format, ...)
{
  va_list valist;
  int res;

  va_start(valist, format);
  res = MSVCRT_fscanf(MSVCRT_stdin, format, valist);
  va_end(valist);
  return res;
}

/*********************************************************************
 *		rename (MSVCRT.@)
 */
int MSVCRT_rename(const char *oldpath,const char *newpath)
{
  TRACE(":from %s to %s\n",oldpath,newpath);
  if (MoveFileExA(oldpath, newpath, MOVEFILE_REPLACE_EXISTING))
    return 0;
  TRACE(":failed (%ld)\n",GetLastError());
  MSVCRT__set_errno(GetLastError());
  return -1;
}

/*********************************************************************
 *		_wrename (MSVCRT.@)
 */
int _wrename(const WCHAR *oldpath,const WCHAR *newpath)
{
  TRACE(":from %s to %s\n",debugstr_w(oldpath),debugstr_w(newpath));
  if (MoveFileExW(oldpath, newpath, MOVEFILE_REPLACE_EXISTING))
    return 0;
  TRACE(":failed (%ld)\n",GetLastError());
  MSVCRT__set_errno(GetLastError());
  return -1;
}

/*********************************************************************
 *		setvbuf (MSVCRT.@)
 */
int MSVCRT_setvbuf(MSVCRT_FILE* file, char *buf, int mode, MSVCRT_size_t size)
{
  FIXME("(%p,%p,%d,%d)stub\n",file, buf, mode, size);
  return -1;
}

/*********************************************************************
 *		setbuf (MSVCRT.@)
 */
void MSVCRT_setbuf(MSVCRT_FILE* file, char *buf)
{
  MSVCRT_setvbuf(file, buf, buf ? MSVCRT__IOFBF : MSVCRT__IONBF, BUFSIZ);
}

/*********************************************************************
 *		tmpnam (MSVCRT.@)
 */
char *MSVCRT_tmpnam(char *s)
{
  char tmpbuf[MAX_PATH];
  char* prefix = "TMP";
  if (!GetTempPathA(MAX_PATH,tmpbuf) ||
      !GetTempFileNameA(tmpbuf,prefix,0,MSVCRT_tmpname))
  {
    TRACE(":failed-last error (%ld)\n",GetLastError());
    return NULL;
  }
  TRACE(":got tmpnam %s\n",MSVCRT_tmpname);
  s = MSVCRT_tmpname;
  return s;
}

/*********************************************************************
 *		tmpfile (MSVCRT.@)
 */
MSVCRT_FILE* MSVCRT_tmpfile(void)
{
  char *filename = MSVCRT_tmpnam(NULL);
  int fd;
  fd = _open(filename, _O_CREAT | _O_BINARY | _O_RDWR | _O_TEMPORARY);
  if (fd != -1)
    return msvcrt_alloc_fp(fd);
  return NULL;
}

/*********************************************************************
 *		vfprintf (MSVCRT.@)
 */
int MSVCRT_vfprintf(MSVCRT_FILE* file, const char *format, va_list valist)
{
  char buf[2048], *mem = buf;
  int written, resize = sizeof(buf), retval;
  /* There are two conventions for vsnprintf failing:
   * Return -1 if we truncated, or
   * Return the number of bytes that would have been written
   * The code below handles both cases
   */
  while ((written = vsnprintf(mem, resize, format, valist)) == -1 ||
          written > resize)
  {
    resize = (written == -1 ? resize * 2 : written + 1);
    if (mem != buf)
      MSVCRT_free (mem);
    if (!(mem = (char *)MSVCRT_malloc(resize)))
      return MSVCRT_EOF;
  }
  retval = MSVCRT_fwrite(mem, 1, written, file);
  if (mem != buf)
    MSVCRT_free (mem);
  return retval;
}

/*********************************************************************
 *		vfwprintf (MSVCRT.@)
 */
int MSVCRT_vfwprintf(MSVCRT_FILE* file, const WCHAR *format, va_list valist)
{
  WCHAR buf[2048], *mem = buf;
  int written, resize = sizeof(buf) / sizeof(WCHAR), retval;
  /* See vfprintf comments */
  while ((written = _vsnwprintf(mem, resize, format, valist)) == -1 ||
          written > resize)
  {
    resize = (written == -1 ? resize * 2 : written + sizeof(WCHAR));
    if (mem != buf)
      MSVCRT_free (mem);
    if (!(mem = (WCHAR *)MSVCRT_malloc(resize)))
      return MSVCRT_EOF;
  }
  retval = MSVCRT_fwrite(mem, 1, written * sizeof (WCHAR), file);
  if (mem != buf)
    MSVCRT_free (mem);
  return retval;
}

/*********************************************************************
 *		vprintf (MSVCRT.@)
 */
int MSVCRT_vprintf(const char *format, va_list valist)
{
  return MSVCRT_vfprintf(MSVCRT_stdout,format,valist);
}

/*********************************************************************
 *		vwprintf (MSVCRT.@)
 */
int MSVCRT_vwprintf(const WCHAR *format, va_list valist)
{
  return MSVCRT_vfwprintf(MSVCRT_stdout,format,valist);
}

/*********************************************************************
 *		fprintf (MSVCRT.@)
 */
int MSVCRT_fprintf(MSVCRT_FILE* file, const char *format, ...)
{
    va_list valist;
    int res;
    va_start(valist, format);
    res = MSVCRT_vfprintf(file, format, valist);
    va_end(valist);
    return res;
}

/*********************************************************************
 *		fwprintf (MSVCRT.@)
 */
int MSVCRT_fwprintf(MSVCRT_FILE* file, const WCHAR *format, ...)
{
    va_list valist;
    int res;
    va_start(valist, format);
    res = MSVCRT_vfwprintf(file, format, valist);
    va_end(valist);
    return res;
}

/*********************************************************************
 *		printf (MSVCRT.@)
 */
int MSVCRT_printf(const char *format, ...)
{
    va_list valist;
    int res;
    va_start(valist, format);
    res = MSVCRT_vfprintf(MSVCRT_stdout, format, valist);
    va_end(valist);
    return res;
}

/*********************************************************************
 *		wprintf (MSVCRT.@)
 */
int MSVCRT_wprintf(const WCHAR *format, ...)
{
    va_list valist;
    int res;
    va_start(valist, format);
    res = MSVCRT_vwprintf(format, valist);
    va_end(valist);
    return res;
}
