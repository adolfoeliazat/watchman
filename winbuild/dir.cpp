/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "watchman_scopeguard.h"
#include "Win32Handle.h"

using watchman::Win32Handle;

namespace {
class WinDirHandle : public watchman_dir_handle {
  Win32Handle h_;
  FILE_FULL_DIR_INFO* info_{nullptr};
  char __declspec(align(8)) buf_[64 * 1024];
  char nameBuf_[WATCHMAN_NAME_MAX];
  struct watchman_dir_ent ent_;

 public:
  explicit WinDirHandle(const char* path, bool strict) {
    int err = 0;
    auto wpath = w_string_piece(path).asWideUNC();

    h_ = Win32Handle(intptr_t(CreateFileW(
        wpath.c_str(),
        GENERIC_READ,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        // Note: FILE_FLAG_OPEN_REPARSE_POINT is equivalent to O_NOFOLLOW,
        // and FILE_FLAG_BACKUP_SEMANTICS is equivalent to O_DIRECTORY
        (strict ? FILE_FLAG_OPEN_REPARSE_POINT : 0) |
            FILE_FLAG_BACKUP_SEMANTICS,
        nullptr)));

    if (!h_) {
      throw std::system_error(
          GetLastError(),
          std::system_category(),
          std::string("CreateFileW for opendir: ") + path);
    }

    memset(&ent_, 0, sizeof(ent_));
    ent_.d_name = nameBuf_;
    ent_.has_stat = true;
    if (path[1] == ':') {
      ent_.stat.dev = tolower(path[0]) - 'a';
    }
  }

  const watchman_dir_ent* readDir() override {
    if (!info_) {
      if (!GetFileInformationByHandleEx(
              (HANDLE)h_.handle(), FileFullDirectoryInfo, buf_, sizeof(buf_))) {
        if (GetLastError() == ERROR_NO_MORE_FILES) {
          return nullptr;
        }
        throw std::system_error(
            GetLastError(),
            std::system_category(),
            "GetFileInformationByHandleEx");
      }
      info_ = (FILE_FULL_DIR_INFO*)buf_;
    }

    // Decode the item currently pointed at
    DWORD len = WideCharToMultiByte(
        CP_UTF8,
        0,
        info_->FileName,
        info_->FileNameLength / sizeof(WCHAR),
        nameBuf_,
        sizeof(nameBuf_) - 1,
        nullptr,
        nullptr);

    if (len <= 0) {
      throw std::system_error(
          GetLastError(), std::system_category(), "WideCharToMultiByte");
    }

    nameBuf_[len] = 0;

    // Populate stat info to speed up the crawler() routine
    FILETIME_LARGE_INTEGER_to_timespec(info_->CreationTime, &ent_.stat.ctime);
    FILETIME_LARGE_INTEGER_to_timespec(info_->LastAccessTime, &ent_.stat.atime);
    FILETIME_LARGE_INTEGER_to_timespec(info_->LastWriteTime, &ent_.stat.mtime);
    ent_.stat.size = info_->EndOfFile.QuadPart;

    if (info_->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      // This is a symlink, but msvcrt has no way to indicate that.
      // We'll treat it as a regular file until we have a better
      // representation :-/
      ent_.stat.mode = _S_IFREG;
    } else if (info_->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      ent_.stat.mode = _S_IFDIR | S_IEXEC | S_IXGRP | S_IXOTH;
    } else {
      ent_.stat.mode = _S_IFREG;
    }
    if (info_->FileAttributes & FILE_ATTRIBUTE_READONLY) {
      ent_.stat.mode |= 0444;
    } else {
      ent_.stat.mode |= 0666;
    }

    // Advance the pointer to the next entry ready for the next read
    info_ = info_->NextEntryOffset == 0
        ? nullptr
        : (FILE_FULL_DIR_INFO*)(((char*)info_) + info_->NextEntryOffset);
    return &ent_;
  }
};
}

std::unique_ptr<watchman_dir_handle> w_dir_open(const char* path, bool strict) {
  return watchman::make_unique<WinDirHandle>(path, strict);
}