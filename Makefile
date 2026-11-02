# GNU Make on Windows. End users install the Setup EXE; these are developer tasks.
.DEFAULT_GOAL := help
ZIG ?= auto
OBJCOPY ?= auto
GH ?= gh.exe
POWERSHELL ?= powershell.exe
VERSION ?= $(shell "$(POWERSHELL)" -NoProfile -Command ". './scripts/Product-Metadata.ps1'; (Get-LakeProductMetadata).Version")

.PHONY: help build test profile installer package release-check publish-check release release-draft graphics-check-build

help:
	"$(POWERSHELL)" -NoProfile -File scripts/Make.ps1 -Target Help

build:
	"$(POWERSHELL)" -NoProfile -File scripts/Make.ps1 -Target Build -Version "$(VERSION)" -ZigPath "$(ZIG)" -ObjcopyPath "$(OBJCOPY)"

test: build

profile:
	"$(POWERSHELL)" -NoProfile -File scripts/Make.ps1 -Target Profile -Version "$(VERSION)" -ZigPath "$(ZIG)" -ObjcopyPath "$(OBJCOPY)"

installer: build
	"$(POWERSHELL)" -NoProfile -File scripts/Make.ps1 -Target Installer -Version "$(VERSION)" -ZigPath "$(ZIG)" -ObjcopyPath "$(OBJCOPY)"

package: installer
	"$(POWERSHELL)" -NoProfile -File scripts/Make.ps1 -Target Package -Version "$(VERSION)"

release-check: package
	"$(POWERSHELL)" -NoProfile -File scripts/Test-Candidate.ps1 -Version "$(VERSION)" -PackageDirectory "out/releases/v$(VERSION)" -AllowUnsignedDevelopment

publish-check: release-check
	"$(POWERSHELL)" -NoProfile -File scripts/Publish-Release.ps1 -Version "$(VERSION)" -GhPath "$(GH)" -DryRun -AllowUnsignedDevelopment

release: package
	"$(POWERSHELL)" -NoProfile -File scripts/Publish-Release.ps1 -Version "$(VERSION)" -GhPath "$(GH)" -AllowUnsignedDevelopment

release-draft: package
	"$(POWERSHELL)" -NoProfile -File scripts/Publish-Release.ps1 -Version "$(VERSION)" -GhPath "$(GH)" -Draft -AllowUnsignedDevelopment

graphics-check-build:
	"$(POWERSHELL)" -NoProfile -File scripts/Build-GraphicsCheck.ps1 -ZigPath "$(ZIG)" -ObjcopyPath "$(OBJCOPY)"
