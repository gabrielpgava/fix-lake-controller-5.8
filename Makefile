# GNU Make on Windows. End users install the Setup EXE; these are developer tasks.
.DEFAULT_GOAL := help
VERSION ?= 0.6.0
ZIG ?= auto
GH ?= gh.exe
POWERSHELL ?= powershell.exe

.PHONY: help build test installer package release-check release release-draft

help:
	"$(POWERSHELL)" -NoProfile -File scripts/Make.ps1 -Target Help

build:
	"$(POWERSHELL)" -NoProfile -File scripts/Make.ps1 -Target Build -Version "$(VERSION)" -ZigPath "$(ZIG)"

test: build

installer: build
	"$(POWERSHELL)" -NoProfile -File scripts/Make.ps1 -Target Installer -Version "$(VERSION)" -ZigPath "$(ZIG)"

package: installer
	"$(POWERSHELL)" -NoProfile -File scripts/Make.ps1 -Target Package -Version "$(VERSION)"

release-check: package
	"$(POWERSHELL)" -NoProfile -File scripts/Publish-Release.ps1 -Version "$(VERSION)" -GhPath "$(GH)" -DryRun

release: package
	"$(POWERSHELL)" -NoProfile -File scripts/Publish-Release.ps1 -Version "$(VERSION)" -GhPath "$(GH)"

release-draft: package
	"$(POWERSHELL)" -NoProfile -File scripts/Publish-Release.ps1 -Version "$(VERSION)" -GhPath "$(GH)" -Draft
