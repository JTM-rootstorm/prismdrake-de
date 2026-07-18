# Copyright 2026 Prismdrake contributors
# Distributed under the terms of the GNU General Public License v3

EAPI=8

PYTHON_COMPAT=( python3_{11..15} )

inherit cmake git-r3 python-any-r1

DESCRIPTION="Experimental Prismdrake PD1 X11 shell and settings foundation"
HOMEPAGE="https://github.com/JTM-rootstorm/prismdrake-de"
EGIT_REPO_URI="https://github.com/JTM-rootstorm/prismdrake-de.git"

LICENSE="GPL-3"
SLOT="0"
KEYWORDS=""
IUSE="test"
RESTRICT="!test? ( test )"

BDEPEND="
	virtual/pkgconfig
	test? (
		${PYTHON_DEPS}
		app-accessibility/at-spi2-core[X,introspection]
		dev-cpp/gtest
		dev-libs/glib
		$(python_gen_any_dep 'dev-python/pygobject[${PYTHON_USEDEP}]')
		gnome-base/gsettings-desktop-schemas
		media-fonts/dejavu
		media-libs/fontconfig
		sys-apps/dbus
		x11-apps/xev
		x11-apps/xprop
		x11-base/xorg-server[xvfb]
		x11-misc/xdotool
		x11-wm/openbox
	)
"
DEPEND="
	>=dev-cpp/nlohmann_json-3.11
	dev-cpp/tomlplusplus
	>=dev-qt/qtbase-6.11:6[X,accessibility,dbus,gui,opengl]
	>=dev-qt/qtdeclarative-6.11:6[accessibility,opengl]
	sys-libs/basu
	x11-libs/libxcb
"
RDEPEND="
	dev-cpp/tomlplusplus
	>=dev-qt/qtbase-6.11:6[X,accessibility,dbus,gui,opengl]
	>=dev-qt/qtdeclarative-6.11:6[accessibility,opengl]
	sys-apps/dbus
	sys-libs/basu
	x11-libs/libxcb
"

python_check_deps() {
	python_has_version -b "dev-python/pygobject[${PYTHON_USEDEP}]"
}

pkg_setup() {
	use test && python-any-r1_pkg_setup
}

src_configure() {
	local mycmakeargs=(
		-DBUILD_TESTING=OFF
		-DPRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST=OFF
		-DPRISMDRAKE_USE_INSTALL_PATHS=ON
	)
	cmake_src_configure
}

src_test() {
	# The product build must retain packaged /usr paths, which do not exist
	# before src_install. Exercise the source tree in a separate test build
	# instead of weakening the installed binary's path contract.
	local BUILD_DIR="${WORKDIR}/${P}_test_build"
	local mycmakeargs=(
		-DBUILD_TESTING=ON
		-DPRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST=ON
		-DPRISMDRAKE_USE_INSTALL_PATHS=OFF
	)
	cmake_src_configure
	cmake_src_compile

	# Two exact-child-environment tests reject Portage's required
	# LD_PRELOAD/SANDBOX_* injection. The two Openbox lanes are also
	# incompatible: libsandbox's LD_PRELOAD changes Openbox client and strut
	# handling. All four are exercised outside the sandbox by the VM suite.
	local sandbox_incompatible_tests="(DetachedApplicationTest.ExecutesExactArgvWorkingDirectoryAndEnvironmentWithoutShell"
	sandbox_incompatible_tests+="|LauncherPipelineTest.ExpandsPlansAndLaunchesLiteralArgumentsWithoutAShell"
	sandbox_incompatible_tests+="|X11DockOpenboxIntegrationTest"
	sandbox_incompatible_tests+="|TaskControllerOpenboxStabilizationIntegrationTest)"
	ctest --test-dir "${BUILD_DIR}" --output-on-failure --parallel 1 \
		--exclude-regex "${sandbox_incompatible_tests}" ||
		die "CTest failed"
}

src_install() {
	cmake_src_install
	dodoc README.md
}

pkg_postinst() {
	elog "This live package installs the Experimental Prismdrake PD1 X11 session skeleton."
	elog "It is a development prototype, not a daily-use desktop environment."
}
