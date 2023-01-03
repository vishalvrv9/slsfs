
testname="$1";
datafile="$2";

tbx services google sheets sheet create \
    -id 1TbUuTnA_vGF1PMDmab1Exw_tmkZp27C7MXVCAB7HsuI \
    -title "$testname"

tbx services google sheets sheet import \
    -id 1TbUuTnA_vGF1PMDmab1Exw_tmkZp27C7MXVCAB7HsuI \
    -data "$datafile" \
    -range "$testname!A1:ZZ100000"
