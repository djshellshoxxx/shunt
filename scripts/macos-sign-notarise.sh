#!/usr/bin/env bash
# Signs Shunt.app with a Developer ID certificate and notarises it (release.yml, tags only).
# Env: MACOS_CERT_P12 (base64), MACOS_CERT_PASSWORD, MACOS_SIGN_IDENTITY,
#      NOTARY_APPLE_ID, NOTARY_TEAM_ID, NOTARY_PASSWORD (app-specific password).
set -euo pipefail
APP="$1"
KEYCHAIN="$RUNNER_TEMP/shunt-signing.keychain-db"
KC_PASS="$(uuidgen)"
echo "$MACOS_CERT_P12" | base64 --decode > "$RUNNER_TEMP/cert.p12"
security create-keychain -p "$KC_PASS" "$KEYCHAIN"
security set-keychain-settings -lut 21600 "$KEYCHAIN"
security unlock-keychain -p "$KC_PASS" "$KEYCHAIN"
security import "$RUNNER_TEMP/cert.p12" -P "$MACOS_CERT_PASSWORD" -A -t cert -f pkcs12 -k "$KEYCHAIN"
security list-keychains -d user -s "$KEYCHAIN" $(security list-keychains -d user | tr -d '"')
security set-key-partition-list -S apple-tool:,apple: -s -k "$KC_PASS" "$KEYCHAIN" >/dev/null

codesign --force --deep --options runtime --timestamp \
  --entitlements "$(dirname "$0")/../installer/macos/Shunt.entitlements" \
  --sign "$MACOS_SIGN_IDENTITY" "$APP"
codesign --verify --strict --verbose=2 "$APP"

ZIP="$RUNNER_TEMP/Shunt-notarise.zip"
ditto -c -k --keepParent "$APP" "$ZIP"
xcrun notarytool submit "$ZIP" --apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" \
  --password "$NOTARY_PASSWORD" --wait
xcrun stapler staple "$APP"
