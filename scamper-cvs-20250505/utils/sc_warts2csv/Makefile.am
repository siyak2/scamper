AUTOMAKE_OPTIONS = subdir-objects

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/scamper

bin_PROGRAMS = sc_warts2csv

sc_warts2csv_SOURCES = \
	sc_warts2csv.c \
	$(top_srcdir)/utils.c

sc_warts2csv_LDADD = \
	$(top_srcdir)/lib/libscamperfile/libscamperfile.la

man_MANS = sc_warts2csv.1

CLEANFILES = *~ *.core
