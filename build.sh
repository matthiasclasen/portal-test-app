#!/usr/bin/env sh

flatpak run org.flatpak.Builder --force-clean --ccache --repo=repo --install --user app org.flatpak.PortalTestAppA.json
flatpak run org.flatpak.Builder --force-clean --ccache --repo=repo --install --user app org.flatpak.PortalTestAppB.json
