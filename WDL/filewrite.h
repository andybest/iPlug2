/*
  WDL - filewrite.h
  Copyright (C) 2005 and later Cockos Incorporated

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
  

  This file provides the WDL_FileWrite object, which can be used to create/write files.
  On windows systems it supports writing synchronously, asynchronously, and asynchronously without buffering.
  On windows systems it supports files larger than 4gb.
  On non-windows systems it acts as a wrapper for fopen()/etc.


*/


#ifndef _WDL_FILEWRITE_H_
#define _WDL_FILEWRITE_H_




#include "ptrlist.h"



#if defined(_WIN32) && !defined(WDL_NO_WIN32_FILEWRITE)
  #ifndef WDL_WIN32_NATIVE_WRITE
    #define WDL_WIN32_NATIVE_WRITE
  #endif
#else
  #ifdef WDL_WIN32_NATIVE_WRITE
    #undef WDL_WIN32_NATIVE_WRITE
  #endif
#endif

#ifdef WDL_WIN32_NATIVE_WRITE

class WDL_FileWrite__WriteEnt
{
public:
  WDL_FileWrite__WriteEnt(int sz)
  {
    m_bufused=0;
    m_bufsz=sz;
    m_bufptr = (char *)__buf.Resize(sz+4095);
    int a=((int)m_bufptr)&4095;
    if (a) m_bufptr += 4096-a;

    memset(&m_ol,0,sizeof(m_ol));
    m_ol.hEvent=CreateEvent(NULL,TRUE,TRUE,NULL);
  }
  ~WDL_FileWrite__WriteEnt()
  {
    CloseHandle(m_ol.hEvent);
  }

  int m_bufused,m_bufsz;
  OVERLAPPED m_ol;
  char *m_bufptr;
  WDL_TypedBuf<char> __buf;
};

#endif



#ifdef _MSC_VER
#define WDL_FILEWRITE_POSTYPE __int64
#else
#define WDL_FILEWRITE_POSTYPE long long
#endif

//#define WIN32_ASYNC_NOBUF_WRITE // this doesnt seem to do much for us, yet.

class WDL_FileWrite
{
public:
  WDL_FileWrite(const char *filename, int allow_async=1, int bufsize=8192, int minbufs=16, int maxbufs=16) // async==2 is unbuffered
  {
    if(!filename)
    {
#ifdef WDL_WIN32_NATIVE_WRITE
      m_fh = INVALID_HANDLE_VALUE;
      m_file_position = 0;
      m_file_max_position=0;
      m_async = 0;
#else
      m_fp = NULL;
#endif
      return;
    }

#ifdef WDL_WIN32_NATIVE_WRITE
    m_async = allow_async && (GetVersion()<0x80000000);
#ifdef WIN32_ASYNC_NOBUF_WRITE
    bufsize = (bufsize+4095)&~4095;
    if (bufsize<4096) bufsize=4096;
#endif

    if (m_async)
#ifdef WIN32_ASYNC_NOBUF_WRITE
      m_fh = CreateFile(filename,GENERIC_WRITE|GENERIC_READ,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED|FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH,NULL);
#else
      m_fh = CreateFile(filename,GENERIC_WRITE|GENERIC_READ,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED|(allow_async>1 ? FILE_FLAG_WRITE_THROUGH: 0),NULL);
#endif
    else
      m_fh = CreateFile(filename,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);

    if (m_async && m_fh != INVALID_HANDLE_VALUE)
    {
      m_async_bufsize=bufsize;
      m_async_maxbufs=maxbufs;
      m_async_minbufs=minbufs;
      int x;
      for (x = 0; x < m_async_minbufs; x ++)
      {
        WDL_FileWrite__WriteEnt *t=new WDL_FileWrite__WriteEnt(m_async_bufsize);
        m_empties.Add(t);
      }
    }

    m_file_position=0;
    m_file_max_position=0;
#else
    m_fp=fopen(filename,"wb");
#endif
  }

  ~WDL_FileWrite()
  {
#ifdef WDL_WIN32_NATIVE_WRITE
    // todo, async close stuff?
    if (m_fh != INVALID_HANDLE_VALUE && m_async)
    {
      RunAsyncWrite();
      SyncOutput();
    }

    m_empties.Empty(true);
    m_pending.Empty(true);

    if (m_fh != INVALID_HANDLE_VALUE) CloseHandle(m_fh);
    m_fh=INVALID_HANDLE_VALUE;
#else
    if (m_fp) fclose(m_fp);
    m_fp=0;
#endif

  }

  bool IsOpen()
  {
#ifdef WDL_WIN32_NATIVE_WRITE
    return (m_fh != INVALID_HANDLE_VALUE);
#else
    return m_fp != NULL;
#endif
  }


  int Write(const void *buf, int len)
  {
#ifdef WDL_WIN32_NATIVE_WRITE
    if (m_fh == INVALID_HANDLE_VALUE) return 0;

    if (m_async)
    {
      char *pbuf=(char *)buf;

      while (len > 0)
      {
        if (!m_empties.GetSize()) 
        {
          WDL_FileWrite__WriteEnt *ent=m_pending.Get(0);
          DWORD s=0;
          if (ent&&GetOverlappedResult(m_fh,&ent->m_ol,&s,FALSE)) 
          {
            ent->m_bufused=0;
            m_pending.Delete(0);
            m_empties.Add(ent);
          }
        }


        WDL_FileWrite__WriteEnt *ent=m_empties.Get(0);
        if (!ent) 
        {
          if (m_pending.GetSize()>=m_async_maxbufs)
          {
            SyncOutput(false);
          }

          if (!(ent=m_empties.Get(0))) 
            m_empties.Add(ent = new WDL_FileWrite__WriteEnt(m_async_bufsize)); // new buffer

          
        }

        int ml=ent->m_bufsz-ent->m_bufused;
        if (ml>len) ml=len;
        memcpy(ent->m_bufptr+ent->m_bufused,pbuf,ml);

        ent->m_bufused+=ml;
        len-=ml;
        pbuf+=ml;

        if (ent->m_bufused >= ent->m_bufsz)
          RunAsyncWrite();
      }
      return pbuf - (char *)buf; 
    }
    else
    {
      DWORD dw=0;
      WriteFile(m_fh,buf,len,&dw,NULL);
      m_file_position+=dw;
      if (m_file_position>m_file_max_position) m_file_max_position=m_file_position;
      return dw;
    }
#else
    return fwrite(buf,1,len,m_fp);
#endif

    
  }

  WDL_FILEWRITE_POSTYPE GetSize()
  {
#ifdef WDL_WIN32_NATIVE_WRITE
    if (m_fh == INVALID_HANDLE_VALUE) return 0;
    DWORD h=0;
    DWORD l=GetFileSize(m_fh,&h);
    WDL_FILEWRITE_POSTYPE tmp=(((WDL_FILEWRITE_POSTYPE)h)<<32)|l;
    if (tmp<m_file_max_position) return m_file_max_position;
    return tmp;
#else
    if (!m_fp) return -1;
    int opos=ftell(m_fp);
    fseek(m_fp,0,SEEK_END);
    int a=ftell(m_fp);
    fseek(m_fp,opos,SEEK_SET);
    return a;

#endif
  }

  WDL_FILEWRITE_POSTYPE GetPosition()
  {
#ifdef WDL_WIN32_NATIVE_WRITE
    if (m_fh == INVALID_HANDLE_VALUE) return -1;

    WDL_FILEWRITE_POSTYPE pos=m_file_position;
    if (m_async)
    {
      WDL_FileWrite__WriteEnt *ent=m_empties.Get(0);
      if (ent) pos+=ent->m_bufused;
    }
    return pos;
#else
    if (!m_fp) return -1;
    return ftell(m_fp);

#endif
  }

#ifdef WDL_WIN32_NATIVE_WRITE

  void RunAsyncWrite()
  {
    WDL_FileWrite__WriteEnt *ent=m_empties.Get(0);
    if (ent && ent->m_bufused>0) 
    {
#ifdef WIN32_ASYNC_NOBUF_WRITE
      if (ent->m_bufused&4095)
      {
        int offs=(ent->m_bufused&4095);
        char tmp[4096];
        memset(tmp,0,4096);

        *(WDL_FILEWRITE_POSTYPE *)&ent->m_ol.Offset = m_file_position + ent->m_bufused - offs;
        ResetEvent(ent->m_ol.hEvent);

        DWORD dw=0;
        if (!ReadFile(m_fh,tmp,4096,&dw,&ent->m_ol))
        {
          if (GetLastError() == ERROR_IO_PENDING) 
            WaitForSingleObject(ent->m_ol.hEvent,INFINITE);
        }
        memcpy(ent->m_bufptr+ent->m_bufused,tmp+offs,4096-offs);

        ent->m_bufused += 4096-offs;
      }
#endif
      DWORD d=0;

      *(WDL_FILEWRITE_POSTYPE *)&ent->m_ol.Offset = m_file_position;
      ResetEvent(ent->m_ol.hEvent);

      int ret=WriteFile(m_fh,ent->m_bufptr,ent->m_bufused,&d,&ent->m_ol);

      m_file_position += ent->m_bufused;
      if (m_file_position>m_file_max_position) m_file_max_position=m_file_position;

      if (ret) // success instantly
      {
        ent->m_bufused=0;
      }
      else
      {
        if (GetLastError()==ERROR_IO_PENDING)
        {
          m_empties.Delete(0);
          m_pending.Add(ent);
        }
        else
        {
//          OutputDebugString("Overlapped file write failed! ouch!\n");
          ent->m_bufused=0;
        }
      }
    }
  }

  void SyncOutput(bool syncall=true)
  {
    for (;;)
    {
      WDL_FileWrite__WriteEnt *ent=m_pending.Get(0);
      if (!ent) break;
      DWORD s=0;
      GetOverlappedResult(m_fh,&ent->m_ol,&s,TRUE);
      ent->m_bufused=0;
      m_pending.Delete(0);
      m_empties.Add(ent);
      if (!syncall) break;
    }
  }

#endif


  bool SetPosition(WDL_FILEWRITE_POSTYPE pos) // returns 0 on success
  {
#ifdef WDL_WIN32_NATIVE_WRITE
    if (m_fh == INVALID_HANDLE_VALUE) return true;
    if (m_async)
    {
      RunAsyncWrite();
      SyncOutput();
      m_file_position=pos;
      if (m_file_position>m_file_max_position) m_file_max_position=m_file_position;

#ifdef WIN32_ASYNC_NOBUF_WRITE
      if (m_file_position&4095)
      {
        WDL_FileWrite__WriteEnt *ent=m_empties.Get(0);
        if (ent)
        {
          int psz=(int) (m_file_position&4095);

          m_file_position -= psz;
          *(WDL_FILEWRITE_POSTYPE *)&ent->m_ol.Offset = m_file_position;
          ResetEvent(ent->m_ol.hEvent);

          DWORD dwo=0;
          if (!ReadFile(m_fh,ent->m_bufptr,4096,&dwo,&ent->m_ol))
          {
            if (GetLastError() == ERROR_IO_PENDING) 
              WaitForSingleObject(ent->m_ol.hEvent,INFINITE);
          }
          ent->m_bufused=(int)psz;
        }
      }
#endif
      return false;
    }

    m_file_position=pos;
    if (m_file_position>m_file_max_position) m_file_max_position=m_file_position;

    LONG high=(LONG) (m_file_position>>32);
    return SetFilePointer(m_fh,(LONG)(m_file_position&0xFFFFFFFFi64),&high,FILE_BEGIN)==0xFFFFFFFF && GetLastError() != NO_ERROR;
#else
    if (!m_fp) return true;
    return !!fseek(m_fp,pos,SEEK_SET);
#endif
  }

#ifdef WDL_WIN32_NATIVE_WRITE
  HANDLE m_fh;
  bool m_async;

  int m_async_bufsize, m_async_minbufs, m_async_maxbufs;

  WDL_PtrList<WDL_FileWrite__WriteEnt> m_empties;
  WDL_PtrList<WDL_FileWrite__WriteEnt> m_pending;

  WDL_FILEWRITE_POSTYPE m_file_position, m_file_max_position;
  
#else
  FILE *m_fp;
#endif
};






#endif
