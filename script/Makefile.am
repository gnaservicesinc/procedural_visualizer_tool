## Process this file with automake to produce Makefile.in
desktopdir 	 = $(datadir)/applications
desktop_in_files = procedural_visualizer_tool.in
desktop_DATA 	 = $(desktop_in_files:.desktop.in=.desktop)
@INTLTOOL_DESKTOP_RULE@

EXTRA_DIST = $(desktop_in_files) \
	procedural_visualizer_tool.desktop.in.in

CLEANFILES = $(desktop_DATA)

DISTCLEANFILES = $(desktop_DATA)

