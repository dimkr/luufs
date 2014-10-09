#!/bin/sh

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
		umount -l c
		rm -rf c b a
		exit 1
	fi
}

mkdir a b c
here="$(pwd)"
./luufs "$here/a" "$here/b" "$here/c"

start_test "Missing file stat"
ls c/dir 2>/dev/null
[ 0 -eq $? ] && end_test 1 || end_test 0

start_test "Duplicate file stat"
cp /bin/sh a/f
cp /bin/sh b/f
echo >> b/f
good_size="$(du -b -D /bin/sh | awk '{print $1}')"
size="$(du -b c/f | awk '{print $1}')"
rm b/f
rm a/f
[ "$size" = "$good_size" ] && end_test 0 || end_test 1

start_test "Directory creation"
mkdir c/dir
end_test $?

start_test "Existing file stat"
ls c/dir > /dev/null
end_test $?

start_test "Directory deletion"
rmdir c/dir
end_test $?

start_test "File deletion"
cp /bin/sh c/f
rm c/f
end_test $?

start_test "Read-only file deletion"
cp /bin/sh a/f
rm c/f 2>/dev/null
ret=$?
rm -f a/f
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Writable file deletion"
cp /bin/sh c/f
rm c/f
end_test $?

start_test "Duplicate file deletion"
cp /bin/sh a/f
cp /bin/sh b/f
rm c/f 2>/dev/null
ret=$?
rm -f b/f
rm -f a/f
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Existing directory creation"
mkdir b/dir
mkdir c/dir 2>/dev/null
ret=$?
rmdir b/dir
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Missing directory deletion"
rm c/dir 2>/dev/null
[ 0 -eq $? ] && end_test 1 || end_test 0

start_test "Read-only directory deletion"
mkdir a/dir
rmdir c/dir 2>/dev/null
ret=$?
rmdir a/dir
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Writeable directory deletion"
mkdir c/dir
rmdir c/dir
end_test $?

start_test "File truncation"
cp /bin/sh c/f
truncate --size 0 c/f
ret=$?
rm c/f
end_test $ret

start_test "Read-only file truncation"
cp /bin/sh a/f
truncate --size 0 c/f 2>/dev/null
ret=$?
rm a/f
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Missing file truncation"
truncate --size 0 c/f
ret=$?
rm c/f
end_test $ret

start_test "Contents listing"
mkdir a/dir
touch b/file
output="$(ls c/)"
rmdir a/dir
rm -f b/file
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
ls c/dir 2>/dev/null
[ 0 -eq $? ] && end_test 1 || end_test 0

start_test "Duplicate file directory contents listing"
mkdir a/dir
mkdir b/dir
output="$(ls c/)"
rmdir b/dir
rmdir a/dir
[ "dir" = "$output" ] && end_test 0 || end_test 1

start_test "Symlink creation"
ln -s x c/y
ret=$?
rm c/y
end_test $ret

start_test "Existing symlink creation"
ln -s x a/y
ln -s w c/y 2>/dev/null
ret=$?
rm a/y
[ 0 -eq $ret ] && end_test 1 || end_test 0

start_test "Symlink deletion"
ln -s x c/y
rm c/y
end_test $?

start_test "Duplicate symlink dereferencing"
ln -s a a/y
ln -s b b/y
output="$(readlink c/y)"
rm b/y
rm a/y
[ "a" = "$output" ] && end_test 0 || end_test 1

echo "All tests passed!"

umount -l c
rm -rf c b a