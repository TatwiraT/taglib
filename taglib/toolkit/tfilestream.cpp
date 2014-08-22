/***************************************************************************
    copyright            : (C) 2002 - 2008 by Scott Wheeler
    email                : wheeler@kde.org
 ***************************************************************************/

/***************************************************************************
 *   This library is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Lesser General Public License version   *
 *   2.1 as published by the Free Software Foundation.                     *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful, but   *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with this library; if not, write to the Free Software   *
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA         *
 *   02110-1301  USA                                                       *
 *                                                                         *
 *   Alternatively, this file is available under the Mozilla Public        *
 *   License Version 1.1.  You may obtain a copy of the License at         *
 *   http://www.mozilla.org/MPL/                                           *
 ***************************************************************************/

#include "tfilestream.h"
#include "tstring.h"
#include "tdebug.h"

#ifdef _WIN32
# include <windows.h>
#else
# include <stdio.h>
# include <unistd.h>
#endif

using namespace TagLib;

namespace
{
#ifdef _WIN32

  // Uses Win32 native API instead of POSIX API to reduce the resource consumption.

  typedef FileName FileNameHandle;
  typedef HANDLE FileHandle;

  const size_t BufferSize = 8192;
  const FileHandle InvalidFileHandle = INVALID_HANDLE_VALUE;

  inline FileHandle openFile(const FileName &path, bool readOnly)
  {
    const DWORD access = readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);

    if(!path.wstr().empty())
      return CreateFileW(path.wstr().c_str(), access, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    else if(!path.str().empty())
      return CreateFileA(path.str().c_str(), access, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    else
      return InvalidFileHandle;
  }

  inline void closeFile(FileHandle file)
  {
    CloseHandle(file);
  }

  inline size_t readFile(FileHandle file, ByteVector &buffer)
  {
    DWORD length;
    if(ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &length, NULL))
      return static_cast<size_t>(length);
    else
      return 0;
  }

  inline size_t writeFile(FileHandle file, const ByteVector &buffer)
  {
    DWORD length;
    if(WriteFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &length, NULL))
      return static_cast<size_t>(length);
    else
      return 0;
  }

#else   // _WIN32

  struct FileNameHandle : public std::string
  {
    FileNameHandle(FileName name) : std::string(name) {}
    operator FileName () const { return c_str(); }
  };

  typedef FILE* FileHandle;

  const size_t BufferSize = 1024;
  const FileHandle InvalidFileHandle = 0;

  inline FileHandle openFile(const FileName &path, bool readOnly)
  {
    return fopen(path, readOnly ? "rb" : "rb+");
  }

  inline void closeFile(FileHandle file)
  {
    fclose(file);
  }

  inline size_t readFile(FileHandle file, ByteVector &buffer)
  {
    return fread(buffer.data(), sizeof(char), buffer.size(), file);
  }

  inline size_t writeFile(FileHandle file, const ByteVector &buffer)
  {
    return fwrite(buffer.data(), sizeof(char), buffer.size(), file);
  }

#endif  // _WIN32
}

class FileStream::FileStreamPrivate
{
public:
  FileStreamPrivate(const FileName &fileName) :
    file(InvalidFileHandle),
    name(fileName),
    readOnly(true) {}

  FileHandle file;
  FileNameHandle name;
  bool readOnly;
};

////////////////////////////////////////////////////////////////////////////////
// public members
////////////////////////////////////////////////////////////////////////////////

FileStream::FileStream(FileName fileName, bool openReadOnly) :
  d(new FileStreamPrivate(fileName))
{
  // First try with read / write mode, if that fails, fall back to read only.

  if(!openReadOnly)
    d->file = openFile(fileName, false);

  if(d->file != InvalidFileHandle)
    d->readOnly = false;
  else
    d->file = openFile(fileName, true);

  if(d->file == InvalidFileHandle)
  {
# ifdef _WIN32
    debug("Could not open file " + fileName.toString());
# else
    debug("Could not open file " + String(static_cast<const char *>(d->name)));
# endif
  }
}

FileStream::~FileStream()
{
  if(isOpen())
    closeFile(d->file);

  delete d;
}

FileName FileStream::name() const
{
  return d->name;
}

ByteVector FileStream::readBlock(size_t length)
{
  if(!isOpen()) {
    debug("FileStream::readBlock() -- invalid file.");
    return ByteVector::null;
  }

  if(length == 0)
    return ByteVector::null;

  const offset_t streamLength = FileStream::length();
  if(length > BufferSize && static_cast<offset_t>(length) > streamLength)
    length = static_cast<size_t>(streamLength);

  ByteVector buffer(length);

  const size_t count = readFile(d->file, buffer);
  buffer.resize(count);

  return buffer;
}

void FileStream::writeBlock(const ByteVector &data)
{
  if(!isOpen()) {
    debug("FileStream::writeBlock() -- invalid file.");
    return;
  }

  if(readOnly()) {
    debug("FileStream::writeBlock() -- read only file.");
    return;
  }

  writeFile(d->file, data);
}

void FileStream::insert(const ByteVector &data, offset_t start, size_t replace)
{
  if(!isOpen()) {
    debug("FileStream::insert() -- invalid file.");
    return;
  }

  if(readOnly()) {
    debug("FileStream::insert() -- read only file.");
    return;
  }

  if(data.size() == replace) {
    seek(start);
    writeBlock(data);
    return;
  }
  else if(data.size() < replace) {
    seek(start);
    writeBlock(data);
    removeBlock(start + data.size(), replace - data.size());
    return;
  }

  // Woohoo!  Faster (about 20%) than id3lib at last.  I had to get hardcore
  // and avoid TagLib's high level API for rendering just copying parts of
  // the file that don't contain tag data.
  //
  // Now I'll explain the steps in this ugliness:

  // First, make sure that we're working with a buffer that is longer than
  // the *differnce* in the tag sizes.  We want to avoid overwriting parts
  // that aren't yet in memory, so this is necessary.

  size_t bufferLength = BufferSize;

  while(data.size() - replace > bufferLength)
    bufferLength += BufferSize;

  // Set where to start the reading and writing.

  offset_t readPosition  = start + replace;
  offset_t writePosition = start;

  ByteVector buffer = data;
  ByteVector aboutToOverwrite(bufferLength);

  while(true)
  {
    // Seek to the current read position and read the data that we're about
    // to overwrite.  Appropriately increment the readPosition.

    seek(readPosition);
    const size_t bytesRead = readFile(d->file, aboutToOverwrite);
    aboutToOverwrite.resize(bytesRead);
    readPosition += bufferLength;

    // Check to see if we just read the last block.  We need to call clear()
    // if we did so that the last write succeeds.

    if(bytesRead < bufferLength)
      clear();

    // Seek to the write position and write our buffer.  Increment the
    // writePosition.

    seek(writePosition);
    writeBlock(buffer);

    // We hit the end of the file.

    if(bytesRead == 0)
      break;

    writePosition += buffer.size();

    // Make the current buffer the data that we read in the beginning.

    buffer = aboutToOverwrite;
  }
}

void FileStream::removeBlock(offset_t start, size_t length)
{
  if(!isOpen()) {
    debug("FileStream::removeBlock() -- invalid file.");
    return;
  }

  size_t bufferLength = BufferSize;

  offset_t readPosition  = start + length;
  offset_t writePosition = start;

  ByteVector buffer(bufferLength, 0);

  for(size_t bytesRead = -1; bytesRead != 0;)
  {
    seek(readPosition);
    bytesRead = readFile(d->file, buffer);
    readPosition += bytesRead;

    // Check to see if we just read the last block.  We need to call clear()
    // if we did so that the last write succeeds.

    if(bytesRead < buffer.size()) {
      clear();
      buffer.resize(bytesRead);
    }

    seek(writePosition);
    writeFile(d->file, buffer);

    writePosition += bytesRead;
  }

  truncate(writePosition);
}

bool FileStream::readOnly() const
{
  return d->readOnly;
}

bool FileStream::isOpen() const
{
  return (d->file != InvalidFileHandle);
}

void FileStream::seek(offset_t offset, Position p)
{
  if(!isOpen()) {
    debug("FileStream::seek() -- invalid file.");
    return;
  }

#ifdef _WIN32

  DWORD whence;
  switch(p) {
  case Beginning:
    whence = FILE_BEGIN;
    break;
  case Current:
    whence = FILE_CURRENT;
    break;
  case End:
    whence = FILE_END;
    break;
  default:
    debug("FileStream::seek() -- Invalid Position value.");
    return;
  }

  LARGE_INTEGER liOffset;
  liOffset.QuadPart = offset;

  SetLastError(NO_ERROR);
  SetFilePointer(d->file, liOffset.LowPart, &liOffset.HighPart, whence);
  if(GetLastError() == ERROR_NEGATIVE_SEEK) {
    SetLastError(NO_ERROR);
    SetFilePointer(d->file, 0, NULL, FILE_BEGIN);
  }
  if(GetLastError() != NO_ERROR) {
    debug("FileStream::seek() -- Failed to set the file pointer.");
  }

#else

  int whence;
  switch(p) {
  case Beginning:
    whence = SEEK_SET;
    break;
  case Current:
    whence = SEEK_CUR;
    break;
  case End:
    whence = SEEK_END;
    break;
  default:
    debug("FileStream::seek() -- Invalid Position value.");
    return;
  }

  fseeko(d->file, offset, whence);

#endif
}

void FileStream::clear()
{
#ifdef _WIN32

  // NOP

#else

  clearerr(d->file);

#endif
}

offset_t FileStream::tell() const
{
#ifdef _WIN32

  LARGE_INTEGER position;
  position.QuadPart = 0;

  SetLastError(NO_ERROR);
  position.LowPart = SetFilePointer(
    d->file, position.LowPart, &position.HighPart, FILE_CURRENT);
  if(GetLastError() == NO_ERROR) {
    return position.QuadPart;
  }
  else {
    debug("FileStream::tell() -- Failed to get the file pointer.");
    return 0;
  }

#else

  return ftello(d->file);

#endif
}

offset_t FileStream::length()
{
  if(!isOpen()) {
    debug("FileStream::length() -- invalid file.");
    return 0;
  }

#ifdef _WIN32

  LARGE_INTEGER fileSize;
  fileSize.QuadPart = 0;

  SetLastError(NO_ERROR);
  fileSize.LowPart = GetFileSize(d->file, reinterpret_cast<LPDWORD>(&fileSize.HighPart));
  if(GetLastError() == NO_ERROR) {
    return fileSize.QuadPart;
  }
  else {
    debug("FileStream::length() -- Failed to get the file size.");
    return 0;
  }

#else

  const offset_t currentPosition = tell();

  seek(0, End);
  offset_t endPosition = tell();

  seek(currentPosition, Beginning);

  return endPosition;

#endif
}

size_t FileStream::bufferSize()
{
  return BufferSize;
}

////////////////////////////////////////////////////////////////////////////////
// protected members
////////////////////////////////////////////////////////////////////////////////

void FileStream::truncate(offset_t length)
{
#ifdef _WIN32

  const offset_t currentPos = tell();

  seek(length);

  SetLastError(NO_ERROR);
  SetEndOfFile(d->file);
  if(GetLastError() != NO_ERROR) {
    debug("FileStream::truncate() -- Failed to truncate the file.");
  }

  seek(currentPos);

#else

  const int error = ftruncate(fileno(d->file), length);
  if(error != 0) {
    debug("FileStream::truncate() -- Coundn't truncate the file.");
  }

#endif
}
