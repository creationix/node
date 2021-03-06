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
#include <string.h>

#include "uv.h"
#include "../uv-common.h"
#include "internal.h"


/* A zero-size buffer for use by uv_pipe_read */
static char uv_zero_[] = "";


int uv_pipe_init(uv_pipe_t* handle) {
  uv_stream_init((uv_stream_t*)handle);

  handle->type = UV_NAMED_PIPE;
  handle->reqs_pending = 0;
  handle->pending_accepts = NULL;
  handle->name = NULL;

  uv_counters()->pipe_init++;

  return 0;
}


static int uv_set_pipe_handle(uv_pipe_t* handle, HANDLE pipeHandle) {
  DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;

  if (!SetNamedPipeHandleState(pipeHandle, &mode, NULL, NULL)) {
    return -1;
  }

  if (CreateIoCompletionPort(pipeHandle,
                             LOOP->iocp,
                             (ULONG_PTR)handle,
                             0) == NULL) {
    return -1;
  }

  return 0;
}


void uv_pipe_endgame(uv_pipe_t* handle) {
  uv_err_t err;
  int status;

  if (handle->flags & UV_HANDLE_SHUTTING &&
      !(handle->flags & UV_HANDLE_SHUT) &&
      handle->write_reqs_pending == 0) {
    close_pipe(handle, &status, &err);

    if (handle->shutdown_req->cb) {
      if (status == -1) {
        LOOP->last_error = err;
      }
      handle->shutdown_req->cb(handle->shutdown_req, status);
    }
    handle->reqs_pending--;
  }

  if (handle->flags & UV_HANDLE_CLOSING &&
      handle->reqs_pending == 0) {
    assert(!(handle->flags & UV_HANDLE_CLOSED));
    handle->flags |= UV_HANDLE_CLOSED;

    if (handle->close_cb) {
      handle->close_cb((uv_handle_t*)handle);
    }

    uv_unref();
  }
}


/* Creates a pipe server. */
/* TODO: make this work with UTF8 name */
int uv_pipe_bind(uv_pipe_t* handle, const char* name) {
  int i;
  uv_pipe_accept_t* req;

  if (!name) {
    uv_set_sys_error(WSAEINVAL);
    return -1;
  }

  /* Make our own copy of the pipe name */
  handle->name = strdup(name);
  if (!handle->name) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  for (i = 0; i < COUNTOF(handle->accept_reqs); i++) {
    req = &handle->accept_reqs[i];
    uv_req_init((uv_req_t*) req);
    req->type = UV_ACCEPT;
    req->data = handle;
    req->pipeHandle = INVALID_HANDLE_VALUE;
    req->next_pending = NULL;
  }

  handle->flags |= UV_HANDLE_PIPESERVER;
  return 0;
}


static DWORD WINAPI pipe_connect_thread_proc(void* parameter) {
  HANDLE pipeHandle = INVALID_HANDLE_VALUE;
  int errno;
  uv_pipe_t* handle;
  uv_connect_t* req;

  req = (uv_connect_t*)parameter;
  assert(req);
  handle = (uv_pipe_t*)req->handle;
  assert(handle);

  /* We're here because CreateFile on a pipe returned ERROR_PIPE_BUSY.  We wait for the pipe to become available with WaitNamedPipe. */
  while (WaitNamedPipe(handle->name, 30000)) {
    /* The pipe is now available, try to connect. */
    pipeHandle = CreateFile(handle->name,
                            GENERIC_READ | GENERIC_WRITE,
                            0,
                            NULL,
                            OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED,
                            NULL);

    if (pipeHandle != INVALID_HANDLE_VALUE) {
      break;
    }
  }

  if (pipeHandle != INVALID_HANDLE_VALUE && !uv_set_pipe_handle(handle, pipeHandle)) {
    handle->handle = pipeHandle;
    req->error = uv_ok_;
  } else {
    req->error = uv_new_sys_error(GetLastError());
  }

  memset(&req->overlapped, 0, sizeof(req->overlapped));

  /* Post completed */
  if (!PostQueuedCompletionStatus(LOOP->iocp,
                                0,
                                0,
                                &req->overlapped)) {
    uv_fatal_error(GetLastError(), "PostQueuedCompletionStatus");
  }

  return 0;
}


/* TODO: make this work with UTF8 name */
int uv_pipe_connect(uv_connect_t* req, uv_pipe_t* handle,
    const char* name, uv_connect_cb cb) {
  int errno;
  HANDLE pipeHandle;

  handle->handle = INVALID_HANDLE_VALUE;

  uv_req_init((uv_req_t*) req);
  req->type = UV_CONNECT;
  req->handle = (uv_stream_t*) handle;
  req->cb = cb;

  pipeHandle = CreateFile(name,
                          GENERIC_READ | GENERIC_WRITE,
                          0,
                          NULL,
                          OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED,
                          NULL);

  if (pipeHandle == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_PIPE_BUSY) {
      /* Wait for the server to make a pipe instance available. */
      handle->name = strdup(name);
      if (!handle->name) {
        uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
      }

      if (!QueueUserWorkItem(&pipe_connect_thread_proc, req, WT_EXECUTELONGFUNCTION)) {
        errno = GetLastError();
        goto error;
      }

      return 0;
    }

    errno = GetLastError();
    goto error;
  }

  if (uv_set_pipe_handle((uv_pipe_t*)req->handle, pipeHandle)) {
    errno = GetLastError();
    goto error;
  }

  handle->handle = pipeHandle;

  req->error = uv_ok_;
  uv_insert_pending_req((uv_req_t*) req);
  handle->reqs_pending++;
  return 0;

error:
  if (pipeHandle != INVALID_HANDLE_VALUE) {
    CloseHandle(pipeHandle);
  }
  uv_set_sys_error(errno);
  return -1;
}


/* Cleans up uv_pipe_t (server or connection) and all resources associated with it */
void close_pipe(uv_pipe_t* handle, int* status, uv_err_t* err) {
  int i;
  HANDLE pipeHandle;

  if (handle->name) {
    free(handle->name);
    handle->name;
  }

  if (handle->flags & UV_HANDLE_PIPESERVER) {
    for (i = 0; i < COUNTOF(handle->accept_reqs); i++) {
      pipeHandle = handle->accept_reqs[i].pipeHandle;
      if (pipeHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(pipeHandle);
      }
    }

  } else if (handle->handle != INVALID_HANDLE_VALUE) {
    CloseHandle(handle->handle);
  }

  handle->flags |= UV_HANDLE_SHUT;
}


static void uv_pipe_queue_accept(uv_pipe_t* handle, uv_pipe_accept_t* req) {
  HANDLE pipeHandle;

  assert(handle->flags & UV_HANDLE_LISTENING);
  assert(req->pipeHandle == INVALID_HANDLE_VALUE);

  pipeHandle = CreateNamedPipe(handle->name,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                               PIPE_UNLIMITED_INSTANCES,
                               65536,
                               65536,
                               0,
                               NULL);

  if (pipeHandle == INVALID_HANDLE_VALUE) {
    req->error = uv_new_sys_error(GetLastError());
    uv_insert_pending_req((uv_req_t*) req);
    handle->reqs_pending++;
    return;
  }

  if (CreateIoCompletionPort(pipeHandle,
                                LOOP->iocp,
                                (ULONG_PTR)handle,
                                0) == NULL) {
    req->error = uv_new_sys_error(GetLastError());
    uv_insert_pending_req((uv_req_t*) req);
    handle->reqs_pending++;
    return;
  }

  /* Prepare the overlapped structure. */
  memset(&(req->overlapped), 0, sizeof(req->overlapped));

  if (!ConnectNamedPipe(pipeHandle, &req->overlapped) && GetLastError() != ERROR_IO_PENDING) {
    if (GetLastError() == ERROR_PIPE_CONNECTED) {
      req->pipeHandle = pipeHandle;
      req->error = uv_ok_;
    } else {
      /* Make this req pending reporting an error. */
      req->error = uv_new_sys_error(GetLastError());
    }
    uv_insert_pending_req((uv_req_t*) req);
    handle->reqs_pending++;
    return;
  }

  req->pipeHandle = pipeHandle;
  handle->reqs_pending++;
}


int uv_pipe_accept(uv_pipe_t* server, uv_pipe_t* client) {
  /* Find a connection instance that has been connected, but not yet accepted. */
  uv_pipe_accept_t* req = server->pending_accepts;

  if (!req) {
    /* No valid connections found, so we error out. */
    uv_set_sys_error(WSAEWOULDBLOCK);
    return -1;
  }

  /* Initialize the client handle and copy the pipeHandle to the client */
  uv_pipe_init(client);
  uv_connection_init((uv_stream_t*) client);
  client->handle = req->pipeHandle;

  /* Prepare the req to pick up a new connection */
  server->pending_accepts = req->next_pending;
  req->next_pending = NULL;
  req->pipeHandle = INVALID_HANDLE_VALUE;

  if (!(server->flags & UV_HANDLE_CLOSING)) {
    uv_pipe_queue_accept(server, req);
  }

  return 0;
}


/* Starts listening for connections for the given pipe. */
int uv_pipe_listen(uv_pipe_t* handle, uv_connection_cb cb) {
  int i, errno;

  if (handle->flags & UV_HANDLE_LISTENING ||
      handle->flags & UV_HANDLE_READING) {
    uv_set_sys_error(UV_EALREADY);
    return -1;
  }

  if (!(handle->flags & UV_HANDLE_PIPESERVER)) {
    uv_set_sys_error(UV_ENOTSUP);
    return -1;
  }

  handle->flags |= UV_HANDLE_LISTENING;
  handle->connection_cb = cb;

  for (i = 0; i < COUNTOF(handle->accept_reqs); i++) {
    uv_pipe_queue_accept(handle, &handle->accept_reqs[i]);
  }

  return 0;
}


static void uv_pipe_queue_read(uv_pipe_t* handle) {
  uv_req_t* req;
  int result;

  assert(handle->flags & UV_HANDLE_READING);
  assert(!(handle->flags & UV_HANDLE_READ_PENDING));

  assert(handle->handle != INVALID_HANDLE_VALUE);

  req = &handle->read_req;
  memset(&req->overlapped, 0, sizeof(req->overlapped));

  /* Do 0-read */
  result = ReadFile(handle->handle,
                    &uv_zero_,
                    0,
                    NULL,
                    &req->overlapped);

  if (!result && GetLastError() != ERROR_IO_PENDING) {
    /* Make this req pending reporting an error. */
    req->error = uv_new_sys_error(WSAGetLastError());
    uv_insert_pending_req(req);
    handle->reqs_pending++;
    return;
  }

  handle->flags |= UV_HANDLE_READ_PENDING;
  handle->reqs_pending++;
}


int uv_pipe_read_start(uv_pipe_t* handle, uv_alloc_cb alloc_cb, uv_read_cb read_cb) {
  if (!(handle->flags & UV_HANDLE_CONNECTION)) {
    uv_set_sys_error(UV_EINVAL);
    return -1;
  }

  if (handle->flags & UV_HANDLE_READING) {
    uv_set_sys_error(UV_EALREADY);
    return -1;
  }

  if (handle->flags & UV_HANDLE_EOF) {
    uv_set_sys_error(UV_EOF);
    return -1;
  }

  handle->flags |= UV_HANDLE_READING;
  handle->read_cb = read_cb;
  handle->alloc_cb = alloc_cb;

  /* If reading was stopped and then started again, there could stell be a */
  /* read request pending. */
  if (!(handle->flags & UV_HANDLE_READ_PENDING))
    uv_pipe_queue_read(handle);

  return 0;
}


int uv_pipe_write(uv_write_t* req, uv_pipe_t* handle, uv_buf_t bufs[], int bufcnt,
    uv_write_cb cb) {
  int result;

  if (bufcnt != 1) {
    uv_set_sys_error(UV_ENOTSUP);
    return -1;
  }

  assert(handle->handle != INVALID_HANDLE_VALUE);

  if (!(handle->flags & UV_HANDLE_CONNECTION)) {
    uv_set_sys_error(UV_EINVAL);
    return -1;
  }

  if (handle->flags & UV_HANDLE_SHUTTING) {
    uv_set_sys_error(UV_EOF);
    return -1;
  }

  uv_req_init((uv_req_t*) req);
  req->type = UV_WRITE;
  req->handle = (uv_stream_t*) handle;
  req->cb = cb;
  memset(&req->overlapped, 0, sizeof(req->overlapped));

  result = WriteFile(handle->handle,
                     bufs[0].base,
                     bufs[0].len,
                     NULL,
                     &req->overlapped);

  if (!result && GetLastError() != WSA_IO_PENDING) {
    uv_set_sys_error(GetLastError());
    return -1;
  }

  if (result) {
    /* Request completed immediately. */
    req->queued_bytes = 0;
  } else {
    /* Request queued by the kernel. */
    req->queued_bytes = uv_count_bufs(bufs, bufcnt);
    handle->write_queue_size += req->queued_bytes;
  }

  handle->reqs_pending++;
  handle->write_reqs_pending++;

  return 0;
}


void uv_process_pipe_read_req(uv_pipe_t* handle, uv_req_t* req) {
  DWORD bytes, err, mode;
  uv_buf_t buf;

  assert(handle->type == UV_NAMED_PIPE);

  handle->flags &= ~UV_HANDLE_READ_PENDING;

  if (req->error.code != UV_OK) {
    /* An error occurred doing the 0-read. */
    if (handle->flags & UV_HANDLE_READING) {
      /* Stop reading and report error. */
      handle->flags &= ~UV_HANDLE_READING;
      LOOP->last_error = req->error;
      buf.base = 0;
      buf.len = 0;
      handle->read_cb((uv_stream_t*)handle, -1, buf);
    }
  } else {
    /*
      * Temporarily switch to non-blocking mode.
      * This is so that ReadFile doesn't block if the read buffer is empty.
      */
    mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(handle->handle, &mode, NULL, NULL)) {
      /* We can't continue processing this read. */
      handle->flags &= ~UV_HANDLE_READING;
      uv_set_sys_error(GetLastError());
      buf.base = 0;
      buf.len = 0;
      handle->read_cb((uv_stream_t*)handle, -1, buf);
    }

    /* Do non-blocking reads until the buffer is empty */
    while (handle->flags & UV_HANDLE_READING) {
      buf = handle->alloc_cb((uv_stream_t*)handle, 65536);
      assert(buf.len > 0);

      if (ReadFile(handle->handle,
                   buf.base,
                   buf.len,
                   &bytes,
                   NULL)) {
        if (bytes > 0) {
          /* Successful read */
          handle->read_cb((uv_stream_t*)handle, bytes, buf);
          /* Read again only if bytes == buf.len */
          if (bytes < buf.len) {
            break;
          }
        } else {
          /* Connection closed */
          handle->flags &= ~UV_HANDLE_READING;
          handle->flags |= UV_HANDLE_EOF;
          LOOP->last_error.code = UV_EOF;
          LOOP->last_error.sys_errno_ = ERROR_SUCCESS;
          handle->read_cb((uv_stream_t*)handle, -1, buf);
          break;
        }
      } else {
        err = GetLastError();
        if (err == ERROR_NO_DATA) {
          /* Read buffer was completely empty, report a 0-byte read. */
          uv_set_sys_error(WSAEWOULDBLOCK);
          handle->read_cb((uv_stream_t*)handle, 0, buf);
        } else {
          /* Ouch! serious error. */
          uv_set_sys_error(err);
          handle->read_cb((uv_stream_t*)handle, -1, buf);
        }
        break;
      }
    }

    /* TODO: if the read callback stops reading we can't start reading again
        because the pipe will still be in nowait mode. */
    if ((handle->flags & UV_HANDLE_READING) &&
        !(handle->flags & UV_HANDLE_READ_PENDING)) {
      /* Switch back to blocking mode so that we can use IOCP for 0-reads */
      mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
      if (SetNamedPipeHandleState(handle->handle, &mode, NULL, NULL)) {
        /* Post another 0-read */
        uv_pipe_queue_read(handle);
      } else {
        /* Report and continue. */
        /* We can't continue processing this read. */
        handle->flags &= ~UV_HANDLE_READING;
        uv_set_sys_error(GetLastError());
        buf.base = 0;
        buf.len = 0;
        handle->read_cb((uv_stream_t*)handle, -1, buf);
      }
    }
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}


void uv_process_pipe_write_req(uv_pipe_t* handle, uv_write_t* req) {
  assert(handle->type == UV_NAMED_PIPE);

  handle->write_queue_size -= req->queued_bytes;

  if (req->cb) {
    LOOP->last_error = req->error;
    ((uv_write_cb)req->cb)(req, LOOP->last_error.code == UV_OK ? 0 : -1);
  }

  handle->write_reqs_pending--;
  if (handle->write_reqs_pending == 0 &&
      handle->flags & UV_HANDLE_SHUTTING) {
    uv_want_endgame((uv_handle_t*)handle);
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}


void uv_process_pipe_accept_req(uv_pipe_t* handle, uv_req_t* raw_req) {
  uv_pipe_accept_t* req = (uv_pipe_accept_t*) raw_req;

  assert(handle->type == UV_NAMED_PIPE);

  if (req->error.code == UV_OK) {
    assert(req->pipeHandle != INVALID_HANDLE_VALUE);

    req->next_pending = handle->pending_accepts;
    handle->pending_accepts = req;

    if (handle->connection_cb) {
      handle->connection_cb((uv_handle_t*)handle, 0);
    }
  } else {
    if (req->pipeHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(req->pipeHandle);
      req->pipeHandle = INVALID_HANDLE_VALUE;
    }
    if (!(handle->flags & UV_HANDLE_CLOSING)) {
      uv_pipe_queue_accept(handle, req);
    }
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}


void uv_process_pipe_connect_req(uv_pipe_t* handle, uv_connect_t* req) {
  assert(handle->type == UV_NAMED_PIPE);

  if (req->cb) {
    if (req->error.code == UV_OK) {
      uv_connection_init((uv_stream_t*)handle);
      ((uv_connect_cb)req->cb)(req, 0);
    } else {
      LOOP->last_error = req->error;
      ((uv_connect_cb)req->cb)(req, -1);
    }
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}
