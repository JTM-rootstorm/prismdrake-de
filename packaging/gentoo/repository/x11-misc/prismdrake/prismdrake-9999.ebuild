# Copyright 2026 Prismdrake contributors
# Distributed under the terms of the GNU General Public License v3

EAPI=8

PYTHON_COMPAT=( python3_{11..15} )

inherit cmake git-r3 python-any-r1

DESCRIPTION="Experimental Prismdrake PD2 core desktop shell"
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
		x11-apps/xrandr
		x11-apps/xwininfo
		x11-base/xorg-server[xvfb]
		x11-drivers/xf86-video-dummy
		x11-misc/xdotool
		x11-misc/xtrace
		x11-themes/hicolor-icon-theme
		x11-wm/openbox
	)
"
DEPEND="
	>=dev-cpp/nlohmann_json-3.11
	dev-cpp/tomlplusplus
	>=dev-qt/qtbase-6.11:6[X,accessibility,dbus,gui,opengl]
	>=dev-qt/qtdeclarative-6.11:6[accessibility,opengl]
	>=dev-qt/qtsvg-6.11:6
	sys-libs/basu
	x11-libs/libxcb
"
RDEPEND="
	dev-cpp/tomlplusplus
	>=dev-qt/qtbase-6.11:6[X,accessibility,dbus,gui,opengl]
	>=dev-qt/qtdeclarative-6.11:6[accessibility,opengl]
	>=dev-qt/qtsvg-6.11:6
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

prismdrake_set_source_date_epoch() {
	local source_date_epoch
	source_date_epoch=$(git -C "${S}" show -s --format=%ct HEAD) ||
		die "could not determine the source revision timestamp"
	[[ ${source_date_epoch} =~ ^[1-9][0-9]*$ ]] ||
		die "source revision timestamp is not a positive integer"
	export SOURCE_DATE_EPOCH=${source_date_epoch}
}

src_configure() {
	prismdrake_set_source_date_epoch
	local mycmakeargs=(
		-DBUILD_TESTING=OFF
		-DPRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST=OFF
		-DPRISMDRAKE_USE_INSTALL_PATHS=ON
	)
	cmake_src_configure
}

src_compile() {
	prismdrake_set_source_date_epoch
	cmake_src_compile
}

src_test() {
	prismdrake_set_source_date_epoch
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
	dodoc \
		README.md \
		docs/architecture/component-model.md \
		docs/architecture/configuration.md \
		docs/architecture/notification-service.md \
		docs/architecture/pd2-state-ownership.md \
		docs/architecture/process-model.md \
		docs/architecture/shell-control.md \
		docs/architecture/surface-placement.md \
		interfaces/README.md
}

pkg_postinst() {
	elog "This live package installs five Experimental PD2 executables:"
	elog "prismdrake-session, prismdrake-settingsd, prismdrake-notifyd,"
	elog "prismdrake-shell, and prismdrake-shellctl."
	elog "Settings1, NotificationCenter1, Shell1, and Session1 remain Experimental."
	elog "It is a development prototype, not a daily-use desktop environment."
}
