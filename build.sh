#!/bin/sh

flatpak-builder --force-clean --ccache --repo=repo --install --user app org.flatpak.PortalTestAppA.json
flatpak-builder --force-clean --ccache --repo=repo --install --user app org.flatpak.PortalTestAppB.json
