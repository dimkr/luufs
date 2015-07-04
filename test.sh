#!/bin/sh

# this file is part of luufs.
#
# Copyright (c) 2014, 2015 Dima Krasner
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

start_test() {
	echo -n "$1 ..."
	sleep 1
}

end_test() {
	if [ 0 -eq $1 ]
	then
		echo " OK"
	else
		echo " failed"
		exit 1
	fi
}

cleanup() {
	umount -l union 2>/dev/null
	rm -rf union rw ro 2>/dev/null
}

mkdir ro rw union
trap cleanup EXIT
trap cleanup INT
trap cleanup TERM

here="$(pwd)"
./luufs "$here/ro" "$here/rw" "$here/union" &

start_test "Missing file stat"
ls union/dir 2>/dev/null
[ 0 -eq $? ] && end_test 1 || end_test 0

start_test "Duplicate file stat"
cp /bin/sh ro/f
cp /bin/sh rw/f
echo >> rw/f
good_size="$(du -b -D /bin/sh | awk '{print $1}')"
size="$(du -b union/f | awk '{print $1}')"
rm rw/f
rm ro/f
[ "$size" = "$good_size" ] && end_test 0 || end_test 1

start_test "Directory creation"
mkdir union/dir
end_test $?

start_test "Existing file stat"
ls union/dir > /dev/null
end_test $?

start_test "Directory deletion"
rmdir union/dir
end_test $?

start_test "File deletion"
cp /bin/sh union/f
rm union/f
end_test $?

start_test "Read-only file deletion"
cp /bin/sh ro/f
rm union/f 2>/dev/null
ret=$?
rm -f ro/f
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Writable file deletion"
cp /bin/sh union/f
rm union/f
end_test $?

start_test "Duplicate file deletion"
cp /bin/sh ro/f
cp /bin/sh rw/f
rm union/f 2>/dev/null
ret=$?
rm -f rw/f
rm -f ro/f
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Existing directory creation"
mkdir rw/dir
mkdir union/dir 2>/dev/null
ret=$?
rmdir rw/dir
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Missing directory deletion"
rm union/dir 2>/dev/null
[ 0 -eq $? ] && end_test 1 || end_test 0

start_test "Read-only directory deletion"
mkdir ro/dir
rmdir union/dir 2>/dev/null
ret=$?
rmdir ro/dir
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Writeable directory deletion"
mkdir union/dir
rmdir union/dir
end_test $?

start_test "File truncation"
cp /bin/sh union/f
truncate --size 0 union/f
ret=$?
rm union/f
end_test $ret

start_test "Read-only file truncation"
cp /bin/sh ro/f
truncate --size 0 union/f 2>/dev/null
ret=$?
rm ro/f
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Missing file truncation"
truncate --size 0 union/f
ret=$?
rm union/f
end_test $ret

start_test "Contents listing"
mkdir ro/dir
touch rw/file
output="$(ls union/)"
rmdir ro/dir
rm -f rw/file
case "$output" in
	*file*)
		case "$output" in
			*dir*)
				end_test 0
				;;
			*)
				end_test 1
				;;
		esac
		;;
	*)
		end_test 1
		;;
esac

start_test "Missing directory contents listing"
ls union/dir 2>/dev/null
[ 0 -eq $? ] && end_test 1 || end_test 0

start_test "Duplicate file directory contents listing"
mkdir ro/dir
mkdir rw/dir
output="$(ls union/)"
rmdir rw/dir
rmdir ro/dir
[ "dir" = "$output" ] && end_test 0 || end_test 1

start_test "Symlink creation"
ln -s x union/y
ret=$?
rm union/y
end_test $ret

start_test "Existing symlink creation"
ln -s x ro/y
ln -s w union/y 2>/dev/null
ret=$?
rm ro/y
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Symlink deletion"
ln -s x union/y
rm union/y
end_test $?

start_test "Duplicate symlink dereferencing"
ln -s a ro/y
ln -s b rw/y
output="$(readlink union/y)"
rm rw/y
rm ro/y
[ "a" = "$output" ] && end_test 0 || end_test 1

echo "All tests passed!"
