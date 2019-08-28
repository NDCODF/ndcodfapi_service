#!/bin/bash
/usr/bin/sqlite3 /usr/local/share/loolwsd/tokens.sqlite 'delete from tokens where (strftime("%s", "now")-expires) > 60 * 60 * 24 * 365; VACUUM FULL;'
