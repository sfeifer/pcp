#!/bin/sh
#
# Copyright (c) 2026 Red Hat.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#
# Rebuild the pmsearch full-text search index from running PMCD.
# Typically invoked nightly via systemd timer or cron.
#

. $PCP_DIR/etc/pcp.env

status=1
tmp=`mktemp -d "$PCP_TMPFILE_DIR/pmsearch_index.XXXXXXXXX"` || exit 1
trap "rm -rf $tmp; exit \$status" 0 1 2 3 15

prog=`basename $0`
INDEX="$PCP_VAR_DIR/lib/pcp.search"

_usage()
{
    echo >&2 "Usage: $prog [-NV?] [-o index]"
    echo >&2 "
Options:
  -N           dry-run, show what would be done
  -o index     output index file path [default: $INDEX]
  -V           verbose diagnostics
  -?           show this usage message"
}

VERBOSE=false
SHOWME=false

ARGS=`pmgetopt --progname=$prog --config=$tmp/config -- \
	-No:V? "$@" 2>/dev/null` || ARGS=""
eval set -- "$ARGS"
while [ $# -gt 0 ]
do
    case "$1"
    in
	-N)	SHOWME=true ;;
	-o)	INDEX="$2"; shift ;;
	-V)	VERBOSE=true ;;
	-?)	_usage; status=0; exit ;;
	--)	shift; break ;;
    esac
    shift
done

if $VERBOSE
then
    echo "$prog: index target: $INDEX"
fi

# Extract all metric help text from PMCD in newhelp format.
# pminfo -tT output format:
#   metricname [oneline text]
#   Help:
#   multi-line helptext
#   <blank line before next metric>
#
# Transform to newhelp format:
#   @ metricname oneline text
#   multi-line helptext
#
pminfo -tT 2>/dev/null | $PCP_AWK_PROG '
/^[a-zA-Z][a-zA-Z0-9_]*\.[a-zA-Z0-9_.]+ / {
    # metric line: "name.with.dots [oneline]" or error
    if (index($0, "One-line Help: Error:") > 0) {
	skip = 1
	next
    }
    skip = 0
    name = $1
    oneline = ""
    start = index($0, "[")
    end = index($0, "]")
    if (start > 0 && end > start) {
	oneline = substr($0, start + 1, end - start - 1)
    }
    printf "@ %s %s\n", name, oneline
    next
}
/^Help:$/ { next }
/^Full Help: Error:/ { skip = 1; next }
skip { next }
{ print }
' > $tmp/helptext

nhelplines=`wc -l < $tmp/helptext`
if [ "$nhelplines" -eq 0 ]
then
    echo >&2 "$prog: warning: no metrics found from PMCD"
    status=0
    exit
fi

if $VERBOSE
then
    nmetrics=`grep -c '^@ ' $tmp/helptext`
    echo "$prog: extracted $nmetrics metrics ($nhelplines lines)"
fi

if $SHOWME
then
    echo "$prog: would run: newhelp -S -o $INDEX $tmp/helptext"
    status=0
    exit
fi

# Ensure output directory exists
outdir=`dirname "$INDEX"`
if [ ! -d "$outdir" ]
then
    mkdir -p "$outdir" 2>/dev/null
    if [ ! -d "$outdir" ]
    then
	echo >&2 "$prog: cannot create directory $outdir"
	exit
    fi
fi

# Build the search index
if newhelp -S -o "$INDEX" $tmp/helptext
then
    if $VERBOSE
    then
	echo "$prog: index written to $INDEX"
    fi
    status=0
else
    echo >&2 "$prog: newhelp -S failed"
fi
