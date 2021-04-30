#!//usr/bin/bash
COUNTER=0

DIRNAME=`dirname $0`
COMMAND=`basename $0`

if test $# -ne 3; then
    echo "syntax: $BASENAME $0 [-a|-s] count url"
    echo "example $BASENAME $0 -a 50 https://example.com"
    echo "example $BASENAME $0 -s 50 https://example.com"
    echo "example $BASENAME $0 -a 50 https://upload.wikimedia.org/wikipedia/commons/d/dd/Big_%26_Small_Pumkins.JPG"
    exit
fi

RUNMOD=$1
MAXCOUNT=$2
GETURL=$3

if [ $RUNMOD != "-a" ] && [ $RUNMOD != "-s" ]
then
    echo "syntax: $BASENAME $0 runmod count url [runmod: async='-a sync='-s']"
    exit
fi

URLS=""

while [ $COUNTER != $MAXCOUNT ]
do
  URLS="$URLS $GETURL"
  COUNTER=$[$COUNTER +1]
done

echo "lanching $DIRNAME/build/curl-http $GETURL (repeated $MAXCOUNT)"
$DIRNAME/build/curl-http -v $RUNMOD $URLS >/dev/null

