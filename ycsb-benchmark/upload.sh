
testname="$1";
datafile="$2";
ID=1fAcBZKe6ansCEEXjMR0L2ogVUrz11oPMHduwDDuOxB4

tbx services google sheets sheet create \
    -id $ID \
    -title "$testname"

tbx services google sheets sheet import \
    -id $ID \
    -data "$datafile" \
    -range "$testname!A1:ZZ100000"
