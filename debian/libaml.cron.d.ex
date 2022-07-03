#
# Regular cron jobs for the libaml package
#
0 4	* * *	root	[ -x /usr/bin/libaml_maintenance ] && /usr/bin/libaml_maintenance
