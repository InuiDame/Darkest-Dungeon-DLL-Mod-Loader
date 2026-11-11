#!/bin/bash
# 用法：runall.sh <exe 名：dd|plain|vanilla> <输出目录> [测试集目录]
v=$1; out=$2; dir=${3:-lua-5.4.9-tests}
cd /d/workplace/DD.ModLoader/build/luatests/$dir || exit 1
B=/d/workplace/DD.ModLoader/build/luatests/standalone/build/windows/x64/release
mkdir -p $out
: > $out/summary_$v.txt
for spec in main gc db calls strings:plain literals:plain tpack attrib gengc locals constructs code:strip cstack nextvar pm utf8 api events vararg closure coroutine goto:strip errors math sort:strip bitwise verybig:strip files; do
  f=${spec%%:*}; m=${spec#*:}; [ "$m" = "$spec" ] && m=dump
  DDSTUB_REPORT=$out/stub_${v}_$f.txt timeout 600 $B/lua_$v.exe ../scripts/runone.lua $f.lua $m > $out/${v}_$f.out 2>&1
  rc=$?
  line=$(grep -m1 "^@@RESULT" $out/${v}_$f.out)
  [ -z "$line" ] && line="@@RESULT $f.lua CRASH rc=$rc $(tail -3 $out/${v}_$f.out | tr '\n' ' ')"
  echo "$line" >> $out/summary_$v.txt
done
cat $out/summary_$v.txt
