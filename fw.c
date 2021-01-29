// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2019-2020  Realtek Corporation
 */

#include "cam.h"
#include "debug.h"
#include "fw.h"
#include "mac.h"
#include "phy.h"
#include "reg.h"

static struct sk_buff *rtw89_fw_h2c_alloc_skb(u32 len, bool header)
{
	struct sk_buff *skb;
	u32 header_len = 0;

	if (header)
		header_len = H2C_HEADER_LEN;

	skb = dev_alloc_skb(len + header_len + 24);
	if (!skb)
		return NULL;
	skb_reserve(skb, header_len + 24);
	memset(skb->data, 0, len);

	return skb;
}

struct sk_buff *rtw89_fw_h2c_alloc_skb_with_hdr(u32 len)
{
	return rtw89_fw_h2c_alloc_skb(len, true);
}

struct sk_buff *rtw89_fw_h2c_alloc_skb_no_hdr(u32 len)
{
	return rtw89_fw_h2c_alloc_skb(len, false);
}

static u8 _fw_get_rdy(struct rtw89_dev *rtwdev)
{
	u8 val = rtw89_read8(rtwdev, R_AX_WCPU_FW_CTRL);

	return FIELD_GET(B_AX_WCPU_FWDL_STS_MASK, val);
}

#define FWDL_WAIT_CNT 400000
int rtw89_fw_check_rdy(struct rtw89_dev *rtwdev)
{
	u8 val;
	int ret;

	ret = read_poll_timeout_atomic(_fw_get_rdy, val,
				       val == RTW89_FWDL_WCPU_FW_INIT_RDY,
				       1, FWDL_WAIT_CNT, false, rtwdev);
	if (ret) {
		switch (val) {
		case RTW89_FWDL_CHECKSUM_FAIL:
			rtw89_err(rtwdev, "fw checksum fail\n");
			return -EINVAL;

		case RTW89_FWDL_SECURITY_FAIL:
			rtw89_err(rtwdev, "fw security fail\n");
			return -EINVAL;

		case RTW89_FWDL_CUT_NOT_MATCH:
			rtw89_err(rtwdev, "fw cut not match\n");
			return -EINVAL;

		default:
			return -EBUSY;
		}
	}

	set_bit(RTW89_FLAG_FW_RDY, rtwdev->flags);

	return 0;
}

static int rtw89_fw_hdr_parser(struct rtw89_dev *rtwdev, const u8 *fw, u32 len,
			       struct rtw89_fw_bin_info *info)
{
	struct rtw89_fw_hdr_section_info *section_info;
	const u8 *fw_end = fw + len;
	const u8 *bin;
	u32 i;

	if (!info)
		return -EINVAL;

	info->section_num = GET_FW_HDR_SEC_NUM(fw);
	info->hdr_len = RTW89_FW_HDR_SIZE +
			info->section_num * RTW89_FW_SECTION_HDR_SIZE;
	SET_FW_HDR_PART_SIZE(fw, FWDL_SECTION_PER_PKT_LEN);

	bin = fw + info->hdr_len;

	/* jump to section header */
	fw += RTW89_FW_HDR_SIZE;
	section_info = info->section_info;
	for (i = 0; i < info->section_num; i++) {
		section_info->len = GET_FWSECTION_HDR_SEC_SIZE(fw);
		if (GET_FWSECTION_HDR_CHECKSUM(fw))
			section_info->len += FWDL_SECTION_CHKSUM_LEN;
		section_info->redl = GET_FWSECTION_HDR_REDL(fw);
		section_info->dladdr =
				GET_FWSECTION_HDR_DL_ADDR(fw) & 0x1fffffff;
		section_info->addr = bin;
		bin += section_info->len;
		fw += RTW89_FW_SECTION_HDR_SIZE;
		section_info++;
	}

	if (fw_end != bin) {
		rtw89_err(rtwdev, "[ERR]fw bin size\n");
		return -EINVAL;
	}

	return 0;
}

static void rtw89_fw_update_ver(struct rtw89_dev *rtwdev,
				const u8 *hdr)
{
	struct rtw89_fw_info *fw_info = &rtwdev->fw;

	fw_info->major_ver = GET_FW_HDR_MAJOR_VERSION(hdr);
	fw_info->minor_ver = GET_FW_HDR_MINOR_VERSION(hdr);
	fw_info->sub_ver = GET_FW_HDR_SUBVERSION(hdr);
	fw_info->sub_idex = GET_FW_HDR_SUBINDEX(hdr);
	fw_info->build_year = GET_FW_HDR_YEAR(hdr);
	fw_info->build_mon = GET_FW_HDR_MONTH(hdr);
	fw_info->build_date = GET_FW_HDR_DATE(hdr);
	fw_info->build_hour = GET_FW_HDR_HOUR(hdr);
	fw_info->build_min = GET_FW_HDR_MIN(hdr);
	fw_info->cmd_ver = GET_FW_HDR_CMD_VERSERION(hdr);
	fw_info->h2c_seq = 0;
	fw_info->rec_seq = 0;

	rtw89_info(rtwdev, "Firmware version %u.%u.%u.%u, CMD version %u\n",
		   fw_info->major_ver, fw_info->minor_ver, fw_info->sub_ver,
		   fw_info->sub_idex, fw_info->cmd_ver);
}

void rtw89_h2c_pkt_set_hdr(struct rtw89_dev *rtwdev, struct sk_buff *skb,
			   u8 type, u8 cat, u8 class, u8 func,
			   bool rack, bool dack, u32 len)
{
	struct fwcmd_hdr *hdr;

	hdr = (struct fwcmd_hdr *)skb_push(skb, 8);

	if (!(rtwdev->fw.h2c_seq % 4))
		rack = true;
	hdr->hdr0 = cpu_to_le32(FIELD_PREP(H2C_HDR_DEL_TYPE, type) |
				FIELD_PREP(H2C_HDR_CAT, cat) |
				FIELD_PREP(H2C_HDR_CLASS, class) |
				FIELD_PREP(H2C_HDR_FUNC, func) |
				FIELD_PREP(H2C_HDR_H2C_SEQ, rtwdev->fw.h2c_seq));

	hdr->hdr1 = cpu_to_le32(FIELD_PREP(H2C_HDR_TOTAL_LEN,
					   len + H2C_HEADER_LEN) |
				(rack ? H2C_HDR_REC_ACK : 0) |
				(dack ? H2C_HDR_DONE_ACK : 0));

	rtwdev->fw.h2c_seq++;
}

static void rtw89_h2c_pkt_set_hdr_fwdl(struct rtw89_dev *rtwdev,
				       struct sk_buff *skb,
				       u8 type, u8 cat, u8 class, u8 func,
				       u32 len)
{
	struct fwcmd_hdr *hdr;

	hdr = (struct fwcmd_hdr *)skb_push(skb, 8);

	hdr->hdr0 = cpu_to_le32(FIELD_PREP(H2C_HDR_DEL_TYPE, type) |
				FIELD_PREP(H2C_HDR_CAT, cat) |
				FIELD_PREP(H2C_HDR_CLASS, class) |
				FIELD_PREP(H2C_HDR_FUNC, func) |
				FIELD_PREP(H2C_HDR_H2C_SEQ, rtwdev->fw.h2c_seq));

	hdr->hdr1 = cpu_to_le32(FIELD_PREP(H2C_HDR_TOTAL_LEN,
					   len + H2C_HEADER_LEN));
}

static int __rtw89_fw_download_hdr(struct rtw89_dev *rtwdev, const u8 *fw, u32 len)
{
	struct sk_buff *skb;
	u32 ret = 0;

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(len);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for fw hdr dl\n");
		return -ENOMEM;
	}

	skb_put_data(skb, fw, len);
	rtw89_h2c_pkt_set_hdr_fwdl(rtwdev, skb, FWCMD_TYPE_H2C,
				   H2C_CAT_MAC, H2C_CL_MAC_FWDL,
				   H2C_FUNC_MAC_FWHDR_DL, len);

	ret = rtw89_h2c_tx(rtwdev, skb, false);
	if (ret) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		ret = -1;
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return ret;
}

static int rtw89_fw_download_hdr(struct rtw89_dev *rtwdev, const u8 *fw, u32 len)
{
	u8 val;
	int ret;

	ret = __rtw89_fw_download_hdr(rtwdev, fw, len);
	if (ret) {
		rtw89_err(rtwdev, "[ERR]FW header download\n");
		return ret;
	}

	ret = read_poll_timeout_atomic(rtw89_read8, val, val & B_AX_FWDL_PATH_RDY,
				       1, FWDL_WAIT_CNT, false,
				       rtwdev, R_AX_WCPU_FW_CTRL);
	if (ret) {
		rtw89_err(rtwdev, "[ERR]FWDL path ready\n");
		return ret;
	}

	rtw89_write32(rtwdev, R_AX_HALT_H2C_CTRL, 0);
	rtw89_write32(rtwdev, R_AX_HALT_C2H_CTRL, 0);

	return 0;
}

static int __rtw89_fw_download_main(struct rtw89_dev *rtwdev,
				    struct rtw89_fw_hdr_section_info *info)
{
	struct sk_buff *skb;
	const u8 *section = info->addr;
	u32 residue_len = info->len;
	u32 pkt_len;
	int ret;

	while (residue_len) {
		if (residue_len >= FWDL_SECTION_PER_PKT_LEN)
			pkt_len = FWDL_SECTION_PER_PKT_LEN;
		else
			pkt_len = residue_len;

		skb = rtw89_fw_h2c_alloc_skb_no_hdr(pkt_len);
		if (!skb) {
			rtw89_err(rtwdev, "failed to alloc skb for fw dl\n");
			return -ENOMEM;
		}
		skb_put_data(skb, section, pkt_len);

		ret = rtw89_h2c_tx(rtwdev, skb, true);
		if (ret) {
			rtw89_err(rtwdev, "failed to send h2c\n");
			ret = -1;
			goto fail;
		}

		section += pkt_len;
		residue_len -= pkt_len;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return ret;
}

static int rtw89_fw_download_main(struct rtw89_dev *rtwdev, const u8 *fw,
				  struct rtw89_fw_bin_info *info)
{
	struct rtw89_fw_hdr_section_info *section_info = info->section_info;
	u8 section_num = info->section_num;
	int ret;

	while (section_num--) {
		ret = __rtw89_fw_download_main(rtwdev, section_info);
		if (ret)
			return ret;
		section_info++;
	}

	mdelay(5);

	ret = rtw89_fw_check_rdy(rtwdev);
	if (ret) {
		rtw89_warn(rtwdev, "download firmware fail\n");
		return ret;
	}

	return 0;
}

static void rtw89_fw_dl_fail_dump(struct rtw89_dev *rtwdev)
{
	u32 val32;
	u16 val16, index;

	val32 = rtw89_read32(rtwdev, R_AX_WCPU_FW_CTRL);
	rtw89_err(rtwdev, "[ERR]fwdl 0x1E0 = 0x%x\n", val32);

	val16 = rtw89_read16(rtwdev, R_AX_BOOT_DBG + 2);
	rtw89_err(rtwdev, "[ERR]fwdl 0x83F2 = 0x%x\n", val16);

	rtw89_write32(rtwdev, R_AX_DBG_CTRL, 0xf200f2);
	rtw89_write32_mask(rtwdev, R_AX_SYS_STATUS1, B_AX_SEL_0XC0, 1);

	for (index = 0; index < 15; index++) {
		val32 = rtw89_read32(rtwdev, R_AX_DBG_PORT_SEL);
		rtw89_err(rtwdev, "[ERR]fw PC = 0x%x\n", val32);
		udelay(10);
	}
}

int rtw89_fw_download(struct rtw89_dev *rtwdev, struct rtw89_fw_info *fw_info)
{
	struct rtw89_fw_bin_info info;
	const u8 *fw = fw_info->firmware->data;
	u32 len = fw_info->firmware->size;
	u8 val;
	int ret;

	ret = rtw89_fw_hdr_parser(rtwdev, fw, len, &info);
	if (ret) {
		rtw89_err(rtwdev, "parse fw header fail\n");
		goto fwdl_err;
	}

	rtw89_fw_update_ver(rtwdev, fw);

	ret = read_poll_timeout_atomic(rtw89_read8, val, val & B_AX_H2C_PATH_RDY,
				       1, FWDL_WAIT_CNT, false,
				       rtwdev, R_AX_WCPU_FW_CTRL);
	if (ret) {
		rtw89_err(rtwdev, "[ERR]H2C path ready\n");
		goto fwdl_err;
	}

	ret = rtw89_fw_download_hdr(rtwdev, fw, info.hdr_len);
	if (ret) {
		ret = -EBUSY;
		goto fwdl_err;
	}

	ret = rtw89_fw_download_main(rtwdev, fw, &info);
	if (ret) {
		ret = -EBUSY;
		goto fwdl_err;
	}

	return ret;

fwdl_err:
	rtw89_fw_dl_fail_dump(rtwdev);
	return ret;
}

int rtw89_wait_firmware_completion(struct rtw89_dev *rtwdev)
{
	struct rtw89_fw_info *fw = &rtwdev->fw;

	wait_for_completion(&fw->completion);
	if (!fw->firmware)
		return -EINVAL;

	return 0;
}

static void rtw89_load_firmware_cb(const struct firmware *firmware, void *context)
{
	struct rtw89_fw_info *fw = context;
	struct rtw89_dev *rtwdev = fw->rtwdev;

	if (!firmware || !firmware->data) {
		rtw89_err(rtwdev, "failed to request firmware\n");
		complete_all(&fw->completion);
		return;
	}

	fw->firmware = firmware;
	complete_all(&fw->completion);
}

int rtw89_load_firmware(struct rtw89_dev *rtwdev)
{
	struct rtw89_fw_info *fw = &rtwdev->fw;
	const char *fw_name = rtwdev->chip->fw_name;
	int ret;

	fw->rtwdev = rtwdev;
	init_completion(&fw->completion);

	ret = request_firmware_nowait(THIS_MODULE, true, fw_name, rtwdev->dev,
				      GFP_KERNEL, fw, rtw89_load_firmware_cb);
	if (ret) {
		rtw89_err(rtwdev, "failed to async firmware request\n");
		return ret;
	}

	return 0;
}

void rtw89_unload_firmware(struct rtw89_dev *rtwdev)
{
	struct rtw89_fw_info *fw = &rtwdev->fw;

	rtw89_wait_firmware_completion(rtwdev);

	if (fw->firmware)
		release_firmware(fw->firmware);
}

#define H2C_CAM_LEN 60
int rtw89_fw_h2c_cam(struct rtw89_dev *rtwdev, struct rtw89_vif *rtwvif)
{
	struct sk_buff *skb;

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(H2C_CAM_LEN);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for fw dl\n");
		return -ENOMEM;
	}
	skb_put(skb, H2C_CAM_LEN);
	rtw89_cam_fill_addr_cam_info(rtwdev, rtwvif, skb->data);
	rtw89_cam_fill_bssid_cam_info(rtwdev, rtwvif, skb->data);

	rtw89_h2c_pkt_set_hdr(rtwdev, skb, FWCMD_TYPE_H2C,
			      H2C_CAT_MAC,
			      H2C_CL_MAC_ADDR_CAM_UPDATE,
			      H2C_FUNC_MAC_ADDR_CAM_UPD, 0, 1,
			      H2C_CAM_LEN);

	if (rtw89_h2c_tx(rtwdev, skb, false)) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return -EBUSY;
}

#define H2C_BA_CAM_LEN 4
int rtw89_fw_h2c_ba_cam(struct rtw89_dev *rtwdev, bool valid, u8 macid,
			struct ieee80211_ampdu_params *params)
{
	struct sk_buff *skb;

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(H2C_BA_CAM_LEN);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for h2c ba cam\n");
		return -ENOMEM;
	}
	skb_put(skb, H2C_BA_CAM_LEN);
	SET_BA_CAM_MACID(skb->data, macid);
	if (!valid)
		goto end;
	SET_BA_CAM_VALID(skb->data, valid);
	SET_BA_CAM_TID(skb->data, params->tid);
	if (params->buf_size > 64)
		SET_BA_CAM_BMAP_SIZE(skb->data, 4);
	else
		SET_BA_CAM_BMAP_SIZE(skb->data, 0);
	/* If init req is set, hw will set the ssn */
	SET_BA_CAM_INIT_REQ(skb->data, 0);
	SET_BA_CAM_SSN(skb->data, params->ssn);

end:
	rtw89_h2c_pkt_set_hdr(rtwdev, skb, FWCMD_TYPE_H2C,
			      H2C_CAT_MAC,
			      H2C_CL_BA_CAM,
			      H2C_FUNC_MAC_BA_CAM, 0, 1,
			      H2C_BA_CAM_LEN);

	if (rtw89_h2c_tx(rtwdev, skb, false)) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return -EBUSY;
}

#define H2C_GENERAL_PKT_LEN 6
#define H2C_GENERAL_PKT_ID_UND 0xff
int rtw89_fw_h2c_general_pkt(struct rtw89_dev *rtwdev, u8 macid)
{
	struct sk_buff *skb;

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(H2C_GENERAL_PKT_LEN);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for fw dl\n");
		return -ENOMEM;
	}
	skb_put(skb, H2C_GENERAL_PKT_LEN);
	SET_GENERAL_PKT_MACID(skb->data, macid);
	SET_GENERAL_PKT_PROBRSP_ID(skb->data, H2C_GENERAL_PKT_ID_UND);
	SET_GENERAL_PKT_PSPOLL_ID(skb->data, H2C_GENERAL_PKT_ID_UND);
	SET_GENERAL_PKT_NULL_ID(skb->data, H2C_GENERAL_PKT_ID_UND);
	SET_GENERAL_PKT_QOS_NULL_ID(skb->data, H2C_GENERAL_PKT_ID_UND);
	SET_GENERAL_PKT_CTS2SELF_ID(skb->data, H2C_GENERAL_PKT_ID_UND);

	rtw89_h2c_pkt_set_hdr(rtwdev, skb, FWCMD_TYPE_H2C,
			      H2C_CAT_MAC,
			      H2C_CL_FW_INFO,
			      H2C_FUNC_MAC_GENERAL_PKT, 0, 1,
			      H2C_GENERAL_PKT_LEN);

	if (rtw89_h2c_tx(rtwdev, skb, false)) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return -EBUSY;
}

#define H2C_CMC_TBL_LEN 68
int rtw89_fw_h2c_default_cmac_tbl(struct rtw89_dev *rtwdev, u8 macid)
{
	struct sk_buff *skb;

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(H2C_CMC_TBL_LEN);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for fw dl\n");
		return -ENOMEM;
	}
	skb_put(skb, H2C_CMC_TBL_LEN);
	SET_CTRL_INFO_MACID(skb->data, macid);
	SET_CTRL_INFO_OPERATION(skb->data, 1);
	SET_CMC_TBL_TXPWR_MODE(skb->data, 0);
	SET_CMC_TBL_NTX_PATH_EN(skb->data, 3);
	SET_CMC_TBL_PATH_MAP_A(skb->data, 0);
	SET_CMC_TBL_PATH_MAP_B(skb->data, 1);
	/* RTW_WKARD_DEF_CMACTBL_CFG */
	SET_CMC_TBL_PATH_MAP_C(skb->data, 0);
	SET_CMC_TBL_PATH_MAP_D(skb->data, 0);
	SET_CMC_TBL_ANTSEL_A(skb->data, 0);
	SET_CMC_TBL_ANTSEL_B(skb->data, 0);
	SET_CMC_TBL_ANTSEL_C(skb->data, 0);
	SET_CMC_TBL_ANTSEL_D(skb->data, 0);
	SET_CMC_TBL_DOPPLER_CTRL(skb->data, 0);
	SET_CMC_TBL_TXPWR_TOLERENCE(skb->data, 0);

	rtw89_h2c_pkt_set_hdr(rtwdev, skb, FWCMD_TYPE_H2C,
			      H2C_CAT_MAC, H2C_CL_MAC_FR_EXCHG,
			      H2C_FUNC_MAC_CCTLINFO_UD, 0, 1,
			      H2C_CMC_TBL_LEN);

	if (rtw89_h2c_tx(rtwdev, skb, false)) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return -EBUSY;
}

static void __get_sta_he_pkt_padding(struct rtw89_dev *rtwdev,
				     struct ieee80211_sta *sta, u8 *pads)
{
	bool ppe_th;
	u8 ppe16, ppe8;
	u8 nss = min(sta->rx_nss, rtwdev->chip->tx_nss) - 1;
	u8 ppe_thres_hdr = sta->he_cap.ppe_thres[0];
	u8 ru_bitmap;
	u8 n, idx, sh;
	u16 ppe;
	int i;

	if (!sta->he_cap.has_he)
		return;

	ppe_th = FIELD_GET(IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT,
			   sta->he_cap.he_cap_elem.phy_cap_info[6]);
	if (!ppe_th) {
		u8 pad;

		pad = FIELD_GET(IEEE80211_HE_PHY_CAP9_NOMIMAL_PKT_PADDING_MASK,
				sta->he_cap.he_cap_elem.phy_cap_info[9]);

		for (i = 0; i < RTW89_PPE_BW_NUM; i++)
			pads[i] = pad;
	}

	ru_bitmap = FIELD_GET(IEEE80211_PPE_THRES_RU_INDEX_BITMASK_MASK, ppe_thres_hdr);
	n = hweight8(ru_bitmap);
	n = 7 + (n * IEEE80211_PPE_THRES_INFO_PPET_SIZE * 2) * nss;

	for (i = 0; i < RTW89_PPE_BW_NUM; i++) {
		if (!(ru_bitmap & BIT(i))) {
			pads[i] = 1;
			continue;
		}

		idx = n >> 3;
		sh = n & 7;
		n += IEEE80211_PPE_THRES_INFO_PPET_SIZE * 2;

		ppe = le16_to_cpu(*((__le16 *)&sta->he_cap.ppe_thres[idx]));
		ppe16 = (ppe >> sh) & IEEE80211_PPE_THRES_NSS_MASK;
		sh += IEEE80211_PPE_THRES_INFO_PPET_SIZE;
		ppe8 = (ppe >> sh) & IEEE80211_PPE_THRES_NSS_MASK;

		if (ppe16 != 7 && ppe8 == 7)
			pads[i] = 2;
		else if (ppe8 != 7)
			pads[i] = 1;
		else
			pads[i] = 0;
	}
}

int rtw89_fw_h2c_assoc_cmac_tbl(struct rtw89_dev *rtwdev,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta)
{
	struct rtw89_sta *rtwsta = (struct rtw89_sta *)sta->drv_priv;
	struct rtw89_vif *rtwvif = (struct rtw89_vif *)vif->drv_priv;
	struct sk_buff *skb;
	u8 pads[RTW89_PPE_BW_NUM];

	memset(pads, 0, sizeof(pads));
	__get_sta_he_pkt_padding(rtwdev, sta, pads);

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(H2C_CMC_TBL_LEN);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for fw dl\n");
		return -ENOMEM;
	}
	skb_put(skb, H2C_CMC_TBL_LEN);
	SET_CTRL_INFO_MACID(skb->data, rtwsta->mac_id);
	SET_CTRL_INFO_OPERATION(skb->data, 1);
	SET_CMC_TBL_DISRTSFB(skb->data, 1);
	SET_CMC_TBL_DISDATAFB(skb->data, 1);
	SET_CMC_TBL_RTS_TXCNT_LMT_SEL(skb->data, 0);
	SET_CMC_TBL_DATA_TXCNT_LMT_SEL(skb->data, 0);
	if (vif->type == NL80211_IFTYPE_STATION)
		SET_CMC_TBL_ULDL(skb->data, 1);
	else
		SET_CMC_TBL_ULDL(skb->data, 0);
	SET_CMC_TBL_MULTI_PORT_ID(skb->data, rtwvif->port);
	SET_CMC_TBL_NOMINAL_PKT_PADDING(skb->data, pads[RTW89_CHANNEL_WIDTH_20]);
	SET_CMC_TBL_NOMINAL_PKT_PADDING40(skb->data, pads[RTW89_CHANNEL_WIDTH_40]);
	SET_CMC_TBL_NOMINAL_PKT_PADDING80(skb->data, pads[RTW89_CHANNEL_WIDTH_80]);

	rtw89_h2c_pkt_set_hdr(rtwdev, skb, FWCMD_TYPE_H2C,
			      H2C_CAT_MAC, H2C_CL_MAC_FR_EXCHG,
			      H2C_FUNC_MAC_CCTLINFO_UD, 0, 1,
			      H2C_CMC_TBL_LEN);

	if (rtw89_h2c_tx(rtwdev, skb, false)) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return -EBUSY;
}

#define H2C_VIF_MAINTAIN_LEN 4
int rtw89_fw_h2c_vif_maintain(struct rtw89_dev *rtwdev,
			      struct rtw89_vif *rtwvif,
			      enum rtw89_upd_mode upd_mode)
{
	struct sk_buff *skb;

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(H2C_VIF_MAINTAIN_LEN);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for h2c join\n");
		return -ENOMEM;
	}
	skb_put(skb, H2C_VIF_MAINTAIN_LEN);
	SET_FWROLE_MAINTAIN_MACID(skb->data, rtwvif->mac_id);
	SET_FWROLE_MAINTAIN_SELF_ROLE(skb->data, rtwvif->self_role);
	SET_FWROLE_MAINTAIN_UPD_MODE(skb->data, upd_mode);
	SET_FWROLE_MAINTAIN_WIFI_ROLE(skb->data, rtwvif->wifi_role);

	rtw89_h2c_pkt_set_hdr(rtwdev, skb, FWCMD_TYPE_H2C,
			      H2C_CAT_MAC, H2C_CL_MAC_MEDIA_RPT,
			      H2C_FUNC_MAC_FWROLE_MAINTAIN, 0, 1,
			      H2C_VIF_MAINTAIN_LEN);

	if (rtw89_h2c_tx(rtwdev, skb, false)) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return -EBUSY;
}

#define H2C_JOIN_INFO_LEN 4
int rtw89_fw_h2c_join_info(struct rtw89_dev *rtwdev, struct rtw89_vif *rtwvif,
			   u8 dis_conn)
{
	struct sk_buff *skb;

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(H2C_JOIN_INFO_LEN);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for h2c join\n");
		return -ENOMEM;
	}
	skb_put(skb, H2C_JOIN_INFO_LEN);
	SET_JOININFO_MACID(skb->data, rtwvif->mac_id);
	SET_JOININFO_OP(skb->data, dis_conn);
	SET_JOININFO_BAND(skb->data, rtwvif->mac_idx);
	SET_JOININFO_WMM(skb->data, rtwvif->wmm);
	SET_JOININFO_TGR(skb->data, rtwvif->trigger);
	SET_JOININFO_ISHESTA(skb->data, 0);
	SET_JOININFO_DLBW(skb->data, 0);
	SET_JOININFO_TF_MAC_PAD(skb->data, 0);
	SET_JOININFO_DL_T_PE(skb->data, 0);
	SET_JOININFO_PORT_ID(skb->data, rtwvif->port);
	SET_JOININFO_NET_TYPE(skb->data, rtwvif->net_type);
	SET_JOININFO_WIFI_ROLE(skb->data, rtwvif->wifi_role);
	SET_JOININFO_SELF_ROLE(skb->data, rtwvif->self_role);

	rtw89_h2c_pkt_set_hdr(rtwdev, skb, FWCMD_TYPE_H2C,
			      H2C_CAT_MAC, H2C_CL_MAC_MEDIA_RPT,
			      H2C_FUNC_MAC_JOININFO, 0, 1,
			      H2C_JOIN_INFO_LEN);

	if (rtw89_h2c_tx(rtwdev, skb, false)) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return -EBUSY;
}

int rtw89_fw_h2c_macid_pause(struct rtw89_dev *rtwdev, u8 sh, u8 grp,
			     bool pause)
{
	struct rtw89_fw_macid_pause_grp h2c = {{0}};
	u8 len = sizeof(struct rtw89_fw_macid_pause_grp);
	struct sk_buff *skb;

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(H2C_JOIN_INFO_LEN);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for h2c join\n");
		return -ENOMEM;
	}
	h2c.mask_grp[grp] = BIT(sh);
	if (pause)
		h2c.pause_grp[grp] = BIT(sh);
	skb_put_data(skb, &h2c, len);

	rtw89_h2c_pkt_set_hdr(rtwdev, skb, FWCMD_TYPE_H2C,
			      H2C_CAT_MAC, H2C_CL_MAC_FW_OFLD,
			      H2C_FUNC_MAC_MACID_PAUSE, 1, 0,
			      len);

	if (rtw89_h2c_tx(rtwdev, skb, false)) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return -EBUSY;
}

#define H2C_RA_LEN 16
int rtw89_fw_h2c_ra(struct rtw89_dev *rtwdev, struct rtw89_ra_info *ra)
{
	struct sk_buff *skb;
	u8 *cmd;

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(H2C_RA_LEN);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for h2c join\n");
		return -ENOMEM;
	}
	skb_put(skb, H2C_RA_LEN);
	cmd = skb->data;
	rtw89_debug(rtwdev, RTW89_DBG_RA,
		    "ra cmd msk: %llx ", ra->ra_mask);

	RTW89_SET_FWCMD_RA_MODE(cmd, ra->mode_ctrl);
	RTW89_SET_FWCMD_RA_BW_CAP(cmd, ra->bw_cap);
	RTW89_SET_FWCMD_RA_MACID(cmd, ra->macid);
	RTW89_SET_FWCMD_RA_DCM(cmd, ra->dcm_cap);
	RTW89_SET_FWCMD_RA_ER(cmd, ra->er_cap);
	RTW89_SET_FWCMD_RA_INIT_RATE_LV(cmd, ra->init_rate_lv);
	RTW89_SET_FWCMD_RA_UPD_ALL(cmd, ra->upd_all);
	RTW89_SET_FWCMD_RA_SGI(cmd, ra->en_sgi);
	RTW89_SET_FWCMD_RA_LDPC(cmd, ra->ldpc_cap);
	RTW89_SET_FWCMD_RA_STBC(cmd, ra->stbc_cap);
	RTW89_SET_FWCMD_RA_SS_NUM(cmd, ra->ss_num);
	RTW89_SET_FWCMD_RA_GILTF(cmd, ra->giltf);
	RTW89_SET_FWCMD_RA_UPD_BW_NSS_MASK(cmd, ra->upd_bw_nss_mask);
	RTW89_SET_FWCMD_RA_UPD_MASK(cmd, ra->upd_mask);
	RTW89_SET_FWCMD_RA_MASK_0(cmd, FIELD_GET(MASKBYTE0, ra->ra_mask));
	RTW89_SET_FWCMD_RA_MASK_1(cmd, FIELD_GET(MASKBYTE1, ra->ra_mask));
	RTW89_SET_FWCMD_RA_MASK_2(cmd, FIELD_GET(MASKBYTE2, ra->ra_mask));
	RTW89_SET_FWCMD_RA_MASK_3(cmd, FIELD_GET(MASKBYTE3, ra->ra_mask));
	RTW89_SET_FWCMD_RA_MASK_4(cmd, FIELD_GET(MASKBYTE4, ra->ra_mask));

	rtw89_h2c_pkt_set_hdr(rtwdev, skb, FWCMD_TYPE_H2C,
			      H2C_CAT_OUTSRC, H2C_CL_OUTSRC_RA,
			      H2C_FUNC_OUTSRC_RA_MACIDCFG, 0, 0,
			      H2C_RA_LEN);

	if (rtw89_h2c_tx(rtwdev, skb, false)) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return -EBUSY;
}

int rtw89_fw_h2c_rf_reg(struct rtw89_dev *rtwdev,
			struct rtw89_fw_h2c_rf_reg_info *info,
			u16 len, u8 page)
{
	struct sk_buff *skb;
	u8 class = info->rf_path == RF_PATH_A ?
		   H2C_CL_OUTSRC_RF_REG_A : H2C_CL_OUTSRC_RF_REG_B;

	skb = rtw89_fw_h2c_alloc_skb_with_hdr(len);
	if (!skb) {
		rtw89_err(rtwdev, "failed to alloc skb for h2c rf reg\n");
		return -ENOMEM;
	}
	skb_put_data(skb, info->rtw89_phy_config_rf_h2c[page], len);

	rtw89_h2c_pkt_set_hdr(rtwdev, skb, FWCMD_TYPE_H2C,
			      H2C_CAT_OUTSRC, class, page, 0, 0,
			      len);

	if (rtw89_h2c_tx(rtwdev, skb, false)) {
		rtw89_err(rtwdev, "failed to send h2c\n");
		goto fail;
	}

	return 0;
fail:
	dev_kfree_skb_any(skb);

	return -EBUSY;
}

void rtw89_fw_c2h_irqsafe(struct rtw89_dev *rtwdev, struct sk_buff *c2h)
{
	skb_queue_tail(&rtwdev->c2h_queue, c2h);
	ieee80211_queue_work(rtwdev->hw, &rtwdev->c2h_work);
}

static void rtw89_fw_c2h_cmd_handle(struct rtw89_dev *rtwdev,
				    struct sk_buff *skb)
{
	u8 category = RTW89_GET_C2H_CATEGORY(skb->data);
	u8 class = RTW89_GET_C2H_CLASS(skb->data);
	u8 func = RTW89_GET_C2H_FUNC(skb->data);
	u16 len = RTW89_GET_C2H_LEN(skb->data);
	bool dump = true;

	if (!test_bit(RTW89_FLAG_RUNNING, rtwdev->flags))
		return;

	switch (category) {
	case RTW89_C2H_CAT_TEST:
		break;
	case RTW89_C2H_CAT_MAC:
		rtw89_mac_c2h_handle(rtwdev, skb, len, class, func);
		if (func == RTW89_MAC_C2H_FUNC_C2H_LOG)
			dump = false;
		break;
	case RTW89_C2H_CAT_OUTSRC:
		rtw89_phy_c2h_handle(rtwdev, skb, len, class, func);
		break;
	}

	if (dump)
		rtw89_hex_dump(rtwdev, RTW89_DBG_FW, "C2H: ", skb->data, skb->len);
}

void rtw89_fw_c2h_work(struct work_struct *work)
{
	struct rtw89_dev *rtwdev = container_of(work, struct rtw89_dev,
						c2h_work);
	struct sk_buff *skb, *tmp;

	skb_queue_walk_safe(&rtwdev->c2h_queue, skb, tmp) {
		skb_unlink(skb, &rtwdev->c2h_queue);
		mutex_lock(&rtwdev->mutex);
		rtw89_fw_c2h_cmd_handle(rtwdev, skb);
		mutex_unlock(&rtwdev->mutex);
		dev_kfree_skb_any(skb);
	}
}

int rtw89_fw_write_h2c_reg(struct rtw89_dev *rtwdev, u32 *h2c_data, u8 h2c_len)
{
	static const u32 h2c_reg[RTW89_H2CREG_MAX] = {
		R_AX_H2CREG_DATA0, R_AX_H2CREG_DATA1,
		R_AX_H2CREG_DATA2, R_AX_H2CREG_DATA3
	};
	u8 i, val;
	int ret;

	ret = read_poll_timeout(rtw89_read8, val, val == 0, 1000, 5000, false,
				rtwdev, R_AX_H2CREG_CTRL);
	if (ret) {
		rtw89_warn(rtwdev, "FW does not process h2c registers\n");
		return ret;
	}

	for (i = 0; i < h2c_len && i < RTW89_H2CREG_MAX; i++)
		rtw89_write32(rtwdev, h2c_reg[i], h2c_data[i]);
	rtw89_write8(rtwdev, R_AX_H2CREG_CTRL, B_AX_H2CREG_TRIGGER);

	ret = read_poll_timeout_atomic(rtw89_read8, val, val, 1,
				       RTW89_C2H_TIMEOUT, false, rtwdev,
				       R_AX_C2HREG_CTRL);
	if (ret) {
		rtw89_warn(rtwdev, "efuse c2h reg timeout\n");
		return ret;
	}

	return 0;
}

int rtw89_fw_read_c2h_reg(struct rtw89_dev *rtwdev,
			  struct rtw89_mac_c2h_info *info)
{
	static const u32 c2h_reg[RTW89_C2HREG_MAX] = {
		R_AX_C2HREG_DATA0, R_AX_C2HREG_DATA1,
		R_AX_C2HREG_DATA2, R_AX_C2HREG_DATA3
	};
	u8 i;

	info->id = RTW89_FWCMD_C2HREG_FUNC_NULL;

	if (rtw89_read8(rtwdev, R_AX_C2HREG_CTRL) == 0) {
		rtw89_warn(rtwdev, "FW does not send c2h reg\n");
		return -EINVAL;
	}

	for (i = 0; i < RTW89_C2HREG_MAX; i++)
		info->c2hreg[i] = rtw89_read32(rtwdev, c2h_reg[i]);

	rtw89_write8(rtwdev, R_AX_C2HREG_CTRL, 0);

	info->id = RTW89_GET_C2H_HDR_FUNC(*info->c2hreg);
	info->content = (u8 *)info->c2hreg + RTW89_C2HREG_HDR_LEN;

	return 0;
}