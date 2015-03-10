#! /bin/bash

CURL=/usr/bin/curl

usage() {
	echo "$0 <file with POST data>"
	exit 1
}

if [ "x$1" = "x" ]; then
	usage;
fi

$CURL --data-urlencode @$1 http://localhost:8080/
echo ""
