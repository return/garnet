// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package wlan

import (
	"fmt"
	mlme "fidl/wlan_mlme"
	"sort"
	"strings"
	"wlan/eapol"
)

type AP struct {
	BSSID        [6]uint8
	SSID         string
	BSSDesc      *mlme.BssDescription
	LastRSSI     uint8
	IsCompatible bool
}

// ByRSSI implements sort.Interface for []AP based on the LastRSSI field.
type ByRSSI []AP

func (a ByRSSI) Len() int           { return len(a) }
func (a ByRSSI) Swap(i, j int)      { a[i], a[j] = a[j], a[i] }
func (a ByRSSI) Less(i, j int) bool { return int8(a[i].LastRSSI) > int8(a[j].LastRSSI) }

func NewAP(bssDesc *mlme.BssDescription) *AP {
	b := *bssDesc // make a copy.
	return &AP{
		BSSID:    bssDesc.Bssid,
		SSID:     bssDesc.Ssid,
		BSSDesc:  &b,
		LastRSSI: 0xff,
	}
}

func macStr(macArray [6]uint8) string {
	return fmt.Sprintf("%02x:%02x:%02x:%02x:%02x:%02x",
		macArray[0], macArray[1], macArray[2], macArray[3], macArray[4], macArray[5])
}

func IsBssCompatible(bss *mlme.BssDescription) bool {
	if bss.Rsn != nil && len(*bss.Rsn) > 0 {
		is_rsn_compatible, err := eapol.IsRSNSupported(*bss.Rsn)
		return is_rsn_compatible && (err == nil)
	}
	return true
}

func CollectScanResults(resp *mlme.ScanConfirm, ssid string, bssid string) []AP {
	aps := []AP{}
	for idx, s := range resp.BssDescriptionSet {
		if bssid != "" {
			// Match the specified BSSID only
			if macStr(s.Bssid) != strings.ToLower(bssid) {
				continue
			}
		}

		if s.Ssid == ssid || ssid == "" {
			ap := NewAP(&s)
			ap.LastRSSI = s.RssiMeasurement
			ap.IsCompatible = IsBssCompatible(&resp.BssDescriptionSet[idx])
			aps = append(aps, *ap)
		}
	}

	if len(resp.BssDescriptionSet) > 0 && len(aps) == 0 {
		fmt.Printf("wlan: no matching network among %d scanned\n", len(resp.BssDescriptionSet))
	}
	sort.Slice(aps, func(i, j int) bool { return int8(aps[i].LastRSSI) > int8(aps[j].LastRSSI) })
	return aps
}
