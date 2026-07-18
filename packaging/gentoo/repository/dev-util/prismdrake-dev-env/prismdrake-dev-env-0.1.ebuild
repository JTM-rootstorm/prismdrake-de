EAPI=8

DESCRIPTION="Development environment for Prismdrake's Gentoo reference VM"
HOMEPAGE="https://github.com/JTM-rootstorm/prismdrake-de"

LICENSE="metapackage"
SLOT="0"
KEYWORDS="~amd64"
IUSE="clang +debug-tools implementation-deps +portage-qa +qt6 visual-tests +x11"

RDEPEND="
	dev-build/cmake
	dev-build/ninja
	dev-util/pkgconf
	dev-vcs/git
	sys-devel/gcc
	clang? ( llvm-core/clang )
	debug-tools? (
		dev-debug/gdb
		dev-debug/strace
		dev-debug/valgrind
		dev-util/perf
		sys-process/lsof
	)
	implementation-deps? (
		dev-cpp/gtest
		dev-cpp/nlohmann_json
		dev-cpp/tomlplusplus
		sys-libs/basu
	)
	portage-qa? (
		app-eselect/eselect-repository
		app-portage/gentoolkit
		app-portage/portage-utils
		dev-util/pkgcheck
		dev-util/pkgdev
	)
	qt6? (
		dev-qt/qtbase:6
		dev-qt/qtdeclarative:6
		dev-qt/qtsvg:6
		dev-qt/qttools:6
	)
	visual-tests? (
		media-gfx/imagemagick
		x11-misc/xdotool
		x11-apps/xwd
	)
	x11? (
		app-accessibility/at-spi2-core
		gnome-base/gsettings-desktop-schemas
		media-fonts/dejavu
		media-libs/fontconfig
		media-libs/mesa
		sys-apps/dbus
		x11-apps/xdpyinfo
		x11-apps/xev
		x11-apps/xhost
		x11-apps/xmessage
		x11-apps/xprop
		x11-apps/xrandr
		x11-apps/xwininfo
		x11-base/xorg-server
		x11-libs/libX11
		x11-libs/libxcb
		x11-libs/libxkbcommon
		x11-libs/xcb-util
		x11-libs/xcb-util-wm
		x11-terms/xterm
		x11-wm/openbox
	)
"
