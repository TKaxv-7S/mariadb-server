/* Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifdef _WIN32
/* You have to use CopyFileEx() and friends manually */
#else
# if defined __APPLE__
/* You should invoke fclonefileat(2) manually before attempting
copy_entire_file() or copy_file() */
#  include <sys/attr.h>
#  include <sys/clonefile.h>
#  include <copyfile.h>
/** Copy an entire file.
@param src  source file descriptor
@param dst  target to append src to
@return error code (negative)
@retval 0   on success */
inline int copy_entire_file(int src, int dst)
{
  return fcopyfile(src, dst, NULL, COPYFILE_ALL | COPYFILE_CLONE);
}
# else
#  ifdef __cplusplus
extern "C"
#  endif
/** Copy an entire file.
@param src  source file descriptor
@param dst  target to append src to
@return error code (negative)
@retval 0   on success */
int copy_entire_file(int src, int dst);
# endif

# ifdef __cplusplus
extern "C"
# endif
/** Copy a file.
@param src   source file descriptor
@param dst   target to append src to
@param start first offset to copy
@param end   last offset to copy (exclusive)
@return error code (negative)
@retval 0   on success */
int copy_file(int src, int dst, off_t start, off_t end);
#endif
