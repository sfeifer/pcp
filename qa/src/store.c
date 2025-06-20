/*
 * Copyright (c) 1997-2001 Silicon Graphics, Inc.  All Rights Reserved.
 */

#include <ctype.h>
#include <pcp/pmapi.h>
#include "libpcp.h"

void
_compare(pmResult *orp, pmResult *nrp)
{
    pmValue		*ovp;
    pmValue		*nvp;
    int			i;
    int			j;
    int			delta;
    int			err = 0;

    if (orp->numpmid != nrp->numpmid) {
	printf("ERROR: numpmid mismatch\n");
	goto bad;
    }

    for (i = 0; i < orp->numpmid; i++) {
	if (orp->vset[i]->numval != nrp->vset[i]->numval) {
	    printf("ERROR: [metric %d] numval mismatch\n", i);
	    err = 1;
	    continue;
	}
	if (orp->vset[i]->valfmt != nrp->vset[i]->valfmt) {
	    printf("ERROR: [metric %d] valfmt mismatch\n", i);
	    err = 1;
	    continue;
	}
	if (orp->vset[i]->valfmt != PM_VAL_INSITU) {
	    printf("ERROR: [metric %d] bogus valfmt\n", i);
	    err = 1;
	    continue;
	}
	for (j = 0; j < orp->vset[i]->numval; j++) {
	    ovp = &orp->vset[i]->vlist[j];
	    nvp = &nrp->vset[i]->vlist[j];
	    delta = ovp->value.lval - nvp->value.lval;
	    if (delta != 0) {
		printf("ERROR: [metric %d][instance %d] value mismatch\n", i, j);
		err = 1;
		if (i == 1 && j == 0)
		    printf("NOTE: this is expected for sampledso.drift\n");
	    }
	}
    }

    if (err)
	goto bad;
    
    return;

bad:

    __pmDumpResult(stdout, orp);
    __pmDumpResult(stdout, nrp);
}

int
main(int argc, char **argv)
{
    int		type = PM_CONTEXT_HOST;
    int		c;
    int		sts;
    int		ctx;
    int		errflag = 0;
    char	*host = "localhost";
    char	*namespace = PM_NS_DEFAULT;
    static char	*usage = "[-x] [-D debugspec] [-h hostname] [-n namespace]";
    int			i;
    int			n;
    const char		*namelist[20];
    pmID		midlist[20];
    int			numpmid;
    pmResult		*old;
    pmResult		*new;
    pmResult		pr;
    pmValueSet		pvs;
    int			guard;

    pmSetProgname(argv[0]);

    while ((c = getopt(argc, argv, "D:h:Ln:")) != EOF) {
	switch (c) {

	case 'D':	/* debug options */
	    sts = pmSetDebug(optarg);
	    if (sts < 0) {
		fprintf(stderr, "%s: unrecognized debug options specification (%s)\n",
		    pmGetProgname(), optarg);
		errflag++;
	    }
	    break;

	case 'h':	/* hostname for PMCD to contact */
	    host = optarg;
	    break;

	case 'L':	/* LOCAL, no PMCD */
	    host = NULL;
	    type = PM_CONTEXT_LOCAL;
	    putenv("PMDA_LOCAL_SAMPLE=");
	    break;

	case 'n':	/* alternative name space file */
	    namespace = optarg;
	    break;

	case '?':
	default:
	    errflag++;
	    break;
	}
    }

    if (errflag) {
	printf("Usage: %s %s\n", pmGetProgname(), usage);
	exit(1);
    }

    if (namespace != PM_NS_DEFAULT && (sts = pmLoadASCIINameSpace(namespace, 1)) < 0) {
	printf("%s: Cannot load namespace from \"%s\": %s\n", pmGetProgname(), namespace, pmErrStr(sts));
	exit(1);
    }

    if ((ctx = pmNewContext(type, host)) < 0) {
	if (type == PM_CONTEXT_HOST)
	    printf("%s: Cannot connect to PMCD on host \"%s\": %s\n",
		pmGetProgname(), host, pmErrStr(ctx));
	else
	    printf("%s: Cannot connect standalone on localhost: %s\n",
		pmGetProgname(), pmErrStr(ctx));
	exit(1);
    }

    i = 0;
    namelist[i++] = "sampledso.write_me";
    namelist[i++] = "sampledso.ulong.write_me";
    namelist[i++] = "sampledso.long.one";
    numpmid = i;
    n = pmLookupName(numpmid, namelist, midlist);
    if (n < 0) {
	printf("pmLookupName: %s\n", pmErrStr(n));
	for (i = 0; i < numpmid; i++) {
	    if (midlist[i] == PM_ID_NULL)
		printf("   %s is bad\n", namelist[i]);
	}
	exit(1);
    }

    if ((n = pmFetch(numpmid, midlist, &old)) < 0) {
	printf("pmFetch old: %s\n", pmErrStr(n));
	exit(1);
    }
    /*
     * need all but last metric to be insitu for the pmStore to work
     */
    for (i = 0; i < numpmid-1; i++) {
	if (old->vset[i]->valfmt != PM_VAL_INSITU) {
	    printf("Botch: metric[%d] %s: not PM_VAL_INSITU\n", i, namelist[i]);
	    exit(1);
	}
    }
    old->numpmid--;
    guard = 0;
again1:
    if ((n = pmStore(old)) < 0) {
	printf("pmStore no change: %s\n", pmErrStr(n));
	if (guard == 0 && n == PM_ERR_TIMEOUT) {
	    /* for qa/866 */
	    pmReconnectContext(ctx);
	    guard++;
	    goto again1;
	}
    }
    if ((n = pmFetch(numpmid - 1, midlist, &new)) < 0) {
	printf("pmFetch new: %s\n", pmErrStr(n));
	exit(1);
    }
    else {
	_compare(old, new);
	pmFreeResult(new);
    }

    old->vset[0]->vlist[0].value.lval++;
    guard = 0;
again2:
    if ((n = pmStore(old)) < 0) {
	printf("pmStore change: %s\n", pmErrStr(n));
	if (guard == 0 && n == PM_ERR_TIMEOUT) {
	    /* for qa/866 */
	    pmReconnectContext(ctx);
	    guard++;
	    goto again2;
	}
    }
    if ((n = pmFetch(numpmid - 1, midlist, &new)) < 0) {
	printf("pmFetch new again: %s\n", pmErrStr(n));
	exit(1);
    }
    else {
	_compare(old, new);
	pmFreeResult(new);
    }

    old->vset[0]->vlist[0].value.lval--;
    guard = 0;
again3:
    if ((n = pmStore(old)) < 0) {
	printf("pmStore restore: %s\n", pmErrStr(n));
	if (guard == 0 && n == PM_ERR_TIMEOUT) {
	    /* for qa/866 */
	    pmReconnectContext(ctx);
	    guard++;
	    goto again3;
	}
    }
    if ((n = pmFetch(numpmid - 1, midlist, &new)) < 0) {
	printf("pmFetch new once more: %s\n", pmErrStr(n));
    }
    else {
	_compare(old, new);
	pmFreeResult(new);
    }

    old->numpmid++;	/* cannot change sampledso.long.one */
    n = pmStore(old);
    guard = 0;
again4:
    if (n != -EACCES && n != PM_ERR_PERMISSION) {
	printf("ERROR: expected EACCES or PM_ERR_PERMISSION error\n");
	printf("pmStore all 3: %s\n", pmErrStr(n));
	if (guard == 0 && n == PM_ERR_TIMEOUT) {
	    /* for qa/866 */
	    pmReconnectContext(ctx);
	    guard++;
	    goto again4;
	}
    }
    pmFreeResult(old);

    /*
     * see http://people.redhat.com/mgoodwin/pcp-cov/1/127dostore.c.html#error
     */
    pr.numpmid = 1;
    pr.vset[0] = &pvs;
    /* assume BROKEN PMDA is not installed */
    pr.vset[0]->pmid = pmID_build(249,123,456);
    pr.vset[0]->numval = 1;
    pr.vset[0]->valfmt = PM_VAL_INSITU;
    pr.vset[0]->vlist[0].inst = PM_INDOM_NULL;
    pr.vset[0]->vlist[0].value.lval = 123456;
    guard = 0;
again5:
    n = pmStore(&pr);
    if (n != PM_ERR_NOAGENT) {
	printf("ERROR: expected PM_ERR_NOAGENT error\n");
	printf("pmStore bad agent domain: %s\n", pmErrStr(n));
	if (guard == 0 && n == PM_ERR_TIMEOUT) {
	    /* for qa/866 */
	    pmReconnectContext(ctx);
	    guard++;
	    goto again5;
	}
	exit(1);
    }

    exit(0);
}
