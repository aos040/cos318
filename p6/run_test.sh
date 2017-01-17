#!/bin/bash

rundir=$(pwd)

function print_usage(){
    echo '$1 :all code path: I will move all the students code in the dir.'
    echo "overview  <all_code_dir>/<studentID>/your code"
    echo "code dir format: A directory named by your StudentID"
    echo "and the dir should contain fs.h/fs.c at least, and also any file you changed on start-code"
    echo "Note: do not add useless files"
    echo "and a new file named <test_parameters>, file format(see usage of p6_test.c): "
    echo "<superblock 1> <superblock 2> <fs_size> <max_inode>"
}


echo "$rundir"
if [ ! -d "$1" ]
then
    print_usage
    exit 0
fi

for code in $(ls $1)
do
    cp $1/$code/* $rundir/
    if [ ! -f "$1/$code/test_parameters" ]
    then
        echo "not found parameters"
        continue
    fi
    sb1=$(cat $1/$code/test_parameters | awk '{print $1}')
    sb2=$(cat $1/$code/test_parameters | awk '{print $2}')
    fs_size=$(cat $1/$code/test_parameters | awk '{print $3}')
    max_inode=$(cat $1/$code/test_parameters | awk '{print $4}')
    
    make clean && make > /dev/null 2>&1
    echo "student : $code test result:" >> all_result.txt
    ./p6_test $sb1 $sb2 $fs_size $max_inode>> all_result.txt
    echo " " >> all_result.txt
done
