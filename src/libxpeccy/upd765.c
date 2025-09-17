#include <string.h>

#include "fdc.h"

#define F_COM_MT	0x80
#define F_COM_MF	0x40
#define F_COM_SK	0x20

#define F_SR1_OR	0x10	// data lost
#define F_SR1_ND	0x04	// no data
#define F_SR1_NW	0x02	// write protect

#ifdef ISDEBUG
#define DBGOUT(args...) printf(args)
#else
#define DBGOUT(args...)
#endif

#define TRBBYTE 1000
#define TRBSRT 1

int seekADR(FDC*);	// from VG93: wait & read ADR mark in fdc->buf

// wait until all args is done
void uwargs(FDC* fdc) {
	if (fdc->comCnt == 0) {
		fdc->irq = 1;		// enter exec phase (idle=0, irq=1)
		fdc->pos++;
	}
	fdc->wait = 1;
}

// do dothing, just spin floppy if motor is on
void unothing(FDC* fdc) {
	if (fdc->flp->motor) {
		flpNext(fdc->flp, fdc->side);
	}
	fdc->wait += fdc->bytedelay; // turbo ? TRBBYTE : fdc->bytedelay;
}

void ustp(FDC* fdc) {
	// fdc->idle = 1;
	fdc->irq = 0;		// enter result phase (idle=0,irq=0)
	fdc->pos++;
	if (fdc->resCnt > 0) {
		fdc->drq = 1;
		fdc->dir = 1;
	} else {
		fdc->dir = 0;
		fdc->drq = 0;
		fdc->idle = 1;
	}
}

void uInt(FDC* fdc) {
	if (!fdc->intr) {
		fdc->xirq(IRQ_FDC, fdc->xptr);
		fdc->intr = 1;
	}
}

void uResp(FDC* fdc, int len, int ie) {
	fdc->resPos = 0;
	fdc->resCnt = len;
	fdc->dir = 1;
	// printf("FDC resp:"); for(int i = 0; i < len; i++) {printf("%.2X ",fdc->resBuf[i]);} printf("\n");	// print resp
	if (ie) {uInt(fdc);}
}

void uSetDrive(FDC* fdc) {
	fdc->flp = fdc->flop[fdc->comBuf[0] & 3];
	fdc->trk = fdc->flp->trk;
	fdc->side = (fdc->comBuf[0] & 4) ? 1 : 0;
	fdc->sr0 &= 0xf8;
	fdc->sr0 |= fdc->comBuf[0] & 7;	// hd & drive
}

int uGetByte(FDC* fdc) {
	int res = 0;
	if (turbo) {
		if (fdc->mr) {		// TC occured: read data, but don't output
			fdc->tmp = flpRd(fdc->flp, fdc->side);
			fdc->tns = 0;
			res = 1;
		} else if (fdc->drq && !fdc->dma) {
			if (fdc->tns > fdc->bytedelay) {
				// prev.data lost
				fdc->sr1 |= 0x10;
				// and get next byte
				fdc->tmp = flpRd(fdc->flp, fdc->side);
				flpNext(fdc->flp, fdc->side);
				fdc->tns = 0;
				res = 1;
			}
		} else {			// data is ready
			fdc->tmp = flpRd(fdc->flp, fdc->side);
			flpNext(fdc->flp, fdc->side);
			fdc->tns = 0;
			res = 1;
		}
		fdc->wait = 1;
	} else {
		if (fdc->drq && !fdc->dma) {
			fdc->sr1 |= 0x10;		// data lost
		}
		fdc->tmp = flpRd(fdc->flp, fdc->side);
		flpNext(fdc->flp, fdc->side);
		fdc->wait += fdc->bytedelay;
		fdc->tns = 0;
		res = 1;
	}
	return res;
}

static fdcCall utermTab[] = {&ustp, &unothing};

void uTerm(FDC* fdc) {
	fdc->plan = utermTab;
	fdc->pos = 0;
	fdc->wait = 1;
	fdc->mr = 0;
}

// 00000011 [SRT:4.HUT:4, HLT:7.ND:1]
// specify

void uspcf00(FDC* fdc) {
	fdc->srt = (((fdc->comBuf[0] >> 4) & 0x0f) + 1) * 1000;
	fdc->hut = (((fdc->comBuf[0] & 0x0f) + 1) << 4) * 1000;
	fdc->hlt = ((fdc->comBuf[1] & 0xfe) + 2) * 1000;
	fdc->dma = (fdc->comBuf[1] & 1) ? 0 : 1;
	fdc->pos++;
}

static fdcCall uSpecify[] = {&uwargs, &uspcf00, &uTerm};

typedef struct {
	int mask;
	int val;
	int argCount;
	fdcCall* plan;
} uCom;

// 00000100 [xxxxx.HD.DRV:2] R [ST3]
// drive status

void udrvst00(FDC* fdc) {
	uSetDrive(fdc);
	fdc->sr3 = (fdc->flp->protect ? 0x40 : 0x00) |
		((fdc->flp->insert && fdc->flp->door) ? 0x20 : 0x00) |
		((fdc->flp->trk == 0) ? 0x10 : 0x00) |
		(fdc->flp->doubleSide ? 0x08 : 0x00) |
		(fdc->comBuf[0] & 7);
	fdc->resBuf[0] = fdc->sr3;
	uResp(fdc, 1, 1);
	fdc->pos++;
}

static fdcCall uDrvStat[] = {&uwargs, &udrvst00, &uTerm};

// 00000111 [xxxxx0.DRV:2]
// recalibrate
// FIXME: too fast when flp is already at track 0

void ucalib00(FDC* fdc) {
	int drv = fdc->comBuf[0] & 3;
	if (fdc->side) fdc->comBuf[0] |= 4;
	uSetDrive(fdc);
	fdc->cnt = 77;			// do 77 step back
	fdc->wait += turbo ? TRBSRT : fdc->srt;
	fdc->state |= (1 << drv);	// set "flp is in seek mode"
	fdc->pos++;
}

void ucalib01(FDC* fdc) {
	if (fdc->flp->trk == 0) {
		fdc->sr0 &= 0x0f;
		fdc->sr0 |= 0x20;	// SR0:0010xxxx
		fdc->state &= 0xf0;
		fdc->wait = fdc->bytedelay * 3;		// wait until ending interrupt
		fdc->pos++;
	} else if (fdc->cnt > 0) {
		flpStep(fdc->flp, FLP_BACK);
		fdc->wait += turbo ? TRBSRT : fdc->srt;
		fdc->cnt--;
	} else {
		fdc->sr0 &= 0x0f;
		fdc->sr0 |= 0x70;	// SR0:0111xxxxx
		fdc->state &= 0xf0;
		fdc->wait = fdc->bytedelay * 3;
		fdc->pos++;
	}
}

void ucalib02(FDC* fdc) {
	uInt(fdc);
	fdc->seekend = 1;
	fdc->pos++;
}

static fdcCall uCalib[] = {&uwargs,&ucalib00,&ucalib01,&ucalib02,&uTerm};

// 00001111 [xxxxx.H.DRV:2,NCN]
// seek

void useek00(FDC* fdc) {
	uSetDrive(fdc);
	// fdc->trk = fdc->flp->trk;		// wait, oh shi...
	fdc->wait += turbo ? TRBSRT : fdc->srt;
	fdc->pos++;
	fdc->state |= (1 << fdc->comBuf[0] & 3);	// set "flp is in seek mode"
}

void useek01(FDC* fdc) {
	if (fdc->trk == fdc->comBuf[1]) {
		fdc->sr0 &= 0x0f;
		fdc->sr0 |= 0x20;
		fdc->pos++;
	} else if (fdc->trk < fdc->comBuf[1]) {
		flpStep(fdc->flp, FLP_FORWARD);
		fdc->wait += turbo ? TRBSRT : fdc->srt;
		fdc->trk++;
	} else {
		flpStep(fdc->flp, FLP_BACK);
		fdc->wait += turbo ? TRBSRT : fdc->srt;
		fdc->trk--;
	}
}

void useek02(FDC* fdc) {
	fdc->state &= 0xf0;		// seek mode off
	fdc->sr0 &= 0x1f;		// b7,6=00:success
	fdc->sr0 |= 0x20;		// seek end
	if (fdc->trk != fdc->flp->trk) {
		fdc->sr0 |= 0x40;	// b7,6=01:abnornal termination
	}
	fdc->wait = fdc->bytedelay * 2;
	fdc->pos++;
}

static fdcCall uSeek[] = {&uwargs,&useek00,&useek01,&useek02,&ucalib02,&uTerm};

// 0.MF.001010 [xxxxx.H.DRV:2] R [ST0,ST1,ST2,C,H,R,N]
// read ID

void urdaRS(FDC* fdc) {
	fdc->resBuf[0] = fdc->sr0;
	fdc->resBuf[1] = fdc->sr1;
	fdc->resBuf[2] = fdc->sr2;
	fdc->resBuf[3] = fdc->buf[0];	// C
	fdc->resBuf[4] = fdc->buf[1];	// H
	fdc->resBuf[5] = fdc->buf[2];	// R
	fdc->resBuf[6] = fdc->buf[3];	// N
	uResp(fdc, 7, 1);
	fdc->pos++;
}

void urda00(FDC* fdc) {
	uSetDrive(fdc);
	if (!(fdc->flp->insert && fdc->flp->door)) {
		fdc->sr0 |= 0x48;
		urdaRS(fdc);
		uTerm(fdc);
	} else {
		fdc->cnt = 2;
		fdc->pos++;
	}
}

void urda01(FDC* fdc) {
	int res = seekADR(fdc);
	if (res == 2) {
		fdc->cnt--;
		if (fdc->cnt == 0) {
			fdc->sr1 |= 0x01;	// missed ADR
			fdc->sr0 |= 0x40;	// exit code 01
			fdc->pos++;
		}
	} else if (res == 1) {
		fdc->pos++;
	}
}

static fdcCall uReadID[] = {&uwargs,&urda00,&urda01,&urdaRS,&uTerm};

// MT.MF.SK.00110 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,DTL] R [SR0,SR1,SR2,C,H,R,N]
// MT.MF.SK.01000 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,DTL] R [SR0,SR1,SR2,C,H,R,N]
// read [deleted] data

void ureadRS(FDC* fdc) {
	fdc->resBuf[0] = fdc->sr0;
	fdc->resBuf[1] = fdc->sr1;
	fdc->resBuf[2] = fdc->sr2;
	fdc->resBuf[3] = fdc->flp->trk;
	fdc->resBuf[4] = fdc->side;
	fdc->resBuf[5] = fdc->sec;
	fdc->resBuf[6] = fdc->buf[3];
	uResp(fdc, 7, 1);
	fdc->pos++;
}

// wait ADR
void uread01(FDC* fdc) {
	int res = seekADR(fdc);
	if (res == 2) {
		fdc->cnt--;
		if (fdc->cnt < 1) {
			fdc->sr0 |= 0x40;	// error
			fdc->sr1 |= 0x04;	// no data
			ureadRS(fdc);
			uTerm(fdc);
		}
	} else if (res == 1) {
		if (fdc->sec == fdc->buf[2]) {		// compare sec.nr
			fdc->cnt = 52;
			fdc->pos++;
		}
	}
}

int ureadCHK(FDC* fdc, int rt, int wt) {
	fdc->tmp = flpRd(fdc->flp, fdc->side);
	flpNext(fdc->flp, fdc->side);
	fdc->wait += turbo ? TRBBYTE : fdc->bytedelay;
	if (fdc->flp->field == rt) return 1;
	if ((fdc->flp->field == wt) && (~fdc->com & F_COM_SK)) {	// wrong data field type, skip off: set b4,sr2, read sector & terminate execution
		fdc->sr2 |= 0x40;
		return 1;
	}
	fdc->cnt--;
	if (fdc->cnt > 0) return 0;	// seek in progress
	fdc->sr0 |= 0x40;		// error
	fdc->sr1 |= 0x04;		// no data
	return 2;
}

void uread02(FDC* fdc) {		// for read data
	int res = (fdc->com & 0x08) ? ureadCHK(fdc,3,2) : ureadCHK(fdc,2,3);
	if (res == 1) {
		fdc->pos++;
		fdc->wait = 1;
	} else if ((res == 2) || fdc->mr) {
		ureadRS(fdc);
		uTerm(fdc);
	}
}

void uread03(FDC* fdc) {
	// TODO: check this moment (data size)
	if (fdc->comBuf[4] & 3) {
		fdc->cnt = 0x80 << (fdc->comBuf[4] & 3);	// N
	} else {
		fdc->cnt = fdc->comBuf[7] + 1;			// DTL
	}
	fdc->state |= 0x10;		// wr/rd operation
	fdc->drq = 0;
	fdc->dir = 1;
	fdc->tns = 0;
	fdc->wait = fdc->bytedelay; // turbo ? TRBBYTE : fdc->bytedelay;
	fdc->pos++;
	// fdc->brk = 1;
	// printf("combuf: %.2X",fdc->com); for (int i = 0; i < 8; i++) {printf(" %.2X",fdc->comBuf[i]);} printf("\n");
}

void uread04(FDC* fdc) {
	if (!uGetByte(fdc)) return;
	if (!fdc->mr) {
		fdc->data = fdc->tmp;
		fdc->drq = 1;
		if (fdc->dma) {
			fdc->xirq(IRQ_FDC_RD, fdc->xptr);
		} else {
			uInt(fdc);
		}
	}
	fdc->cnt--;
	if (fdc->cnt < 1) {
		fdc->pos++;
		fdc->wait = fdc->bytedelay; // turbo ? TRBBYTE : fdc->bytedelay;
	}
}

// check EOT, MT
void uread05(FDC* fdc) {
	fdc->state &= ~0x10;
	fdc->sec++;
	if (fdc->mr || (fdc->sr2 & 0x40)) {				// SR2:CM flag set - not right sector & SK=0... or TC
		fdc->pos++;
	} else if (fdc->sec > fdc->comBuf[5]) {				// check EOT
		if ((fdc->com & F_COM_MT) && !fdc->side) {		// multitrack (side 0->1 only)
			fdc->side = 1;
			fdc->cnt = 2;
			fdc->sec = 1;
			fdc->pos = 0;
		} else {
			fdc->pos++;
		}
	} else {
		fdc->cnt = 2;
		fdc->pos = 0;
	}
}

static fdcCall uReadD01[] = {&uread01,&uread02,&uread03,&uread04,&uread05,&ureadRS,&uTerm};

void uread00(FDC* fdc) {
	uSetDrive(fdc);
// NOTE: multitrack means select hd1 after hd0 is done, not to read hd0 from start
	fdc->sec = fdc->comBuf[3];	// R, sec.num
	if (!(fdc->flp->insert && fdc->flp->door)) {
		fdc->sr0 |= 0x48;	// not ready
		ureadRS(fdc);		// prepare resp
		uTerm(fdc);		// terminate
	} else {
		fdc->cnt = 2;
		fdc->wait += turbo ? TRBBYTE : fdc->hlt;	// load head
		fdc->pos++;
	}
}

void uread00a(FDC* fdc) {
	fdc->plan = uReadD01;
	fdc->pos = 0;
}

static fdcCall uRdData[] = {&uwargs,&uread00,&uread00a};

// 0.MF.SK.00010 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,DTL] R [SR0,SR1,SR2,C,H,R,N]
// read track

void urdtrkRS(FDC* fdc) {
	if (!(fdc->flp->insert && fdc->flp->door))
		fdc->sr0 |= 0x08;
	fdc->resBuf[0] = fdc->sr0;
	fdc->resBuf[1] = fdc->sr1;
	fdc->resBuf[2] = fdc->sr2;
	fdc->resBuf[3] = fdc->flp->trk;
	fdc->resBuf[4] = fdc->side;
	fdc->resBuf[5] = fdc->sec;
	fdc->resBuf[6] = fdc->buf[3];
	uResp(fdc, 7, 1);
	fdc->pos++;
}

void urdtrk00(FDC* fdc) {
	uSetDrive(fdc);
	fdc->sec = fdc->comBuf[3];		// R, sec.num
	if (!(fdc->flp->insert && fdc->flp->door)) {
		fdc->sr0 |= 0x48;	// not ready
		urdtrkRS(fdc);		// prepare resp
		uTerm(fdc);		// terminate
	} else {
		fdc->sec = 0;
		fdc->sr1 |= 0x05;	// MA (reset on 1st found sector), ND (reset if specified sector found)
		fdc->pos++;
		fdc->wait = 0;
	}
}

// wait IDX
void urdtrk01(FDC* fdc) {
	if (seekADR(fdc) == 2)
		fdc->pos++;
}

// find ADR
void urdtrk02(FDC* fdc) {
	int res = seekADR(fdc);
	if (res == 2) {				// 2nd IDX
		if (fdc->sr1 & 0x01)		// if no ADR was found, error
			fdc->sr0 |= 0x40;
		urdtrkRS(fdc);
		uTerm(fdc);
	} else if (res == 1) {
		fdc->sr1 &= ~0x01;			// reset MA
		if (fdc->buf[2] == fdc->comBuf[3]) {	// if specified sector found, reset ND
			fdc->sr1 &= ~0x04;
		}
		fdc->cnt = 53;
		fdc->pos++;
	}
}

// find DATA
void urdtrk03(FDC* fdc) {
	int res = ureadCHK(fdc,2,3);
	if (res == 2) {				// IDX : end
		urdtrkRS(fdc);
		uTerm(fdc);
	} else {
		fdc->pos++;
		fdc->wait = 0;
	}
}

// uread03: prepare to data transfer
// uread04: transfer data FDD->FDC->CPU
// go next sector
void urdtrk04(FDC* fdc) {
	fdc->sec++;
	if (fdc->sec >= fdc->comBuf[5]) {	// EOT
		fdc->pos++;
	} else {
		fdc->pos = 3;
	}
}

static fdcCall uRdTrk[] = {&uwargs, &urdtrk00, &urdtrk01, &urdtrk02, &urdtrk03, &uread03, &uread04, &urdtrk04, &urdtrkRS, &uTerm};

// MT.MF.SK.10001 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,STP] R [SR0,SR1,SR2,C,H,R,N]	scan equal
// MT.MF.SK.11001 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,STP] R [SR0,SR1,SR2,C,H,R,N]	scan low or equal
// MT.MF.SK.11101 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,STP] R [SR0,SR1,SR2,C,H,R,N]	scan high or equal

// uread00 : prepare FDC, fdc->sec = R
// uread01 : find ADR for sector C in 2 turns
// find DATA
void uscan00(FDC* fdc) {
	int res = ureadCHK(fdc,2,3);
	if (res == 1) {
		fdc->pos++;
		fdc->wait = 0;
	} else if (res == 2) {
		ureadRS(fdc);
		uTerm(fdc);
	}
}

// prepare compare
void uscan01(FDC* fdc) {
	fdc->sr2 &= 0xf3;
	fdc->sr2 |= 0x08;	// SH=1, SN=0
	fdc->cnt = 0x80 << (fdc->buf[3] & 3);
	fdc->drq = 0;
	fdc->dir = 1;
	fdc->wait = fdc->bytedelay; // turbo ? TRBBYTE : fdc->bytedelay;
	fdc->pos++;
}

// compare whole sector
void uscan02(FDC* fdc) {
	if (!uGetByte(fdc)) return;
	fdc->drq = 1;
	if (fdc->tmp != fdc->data)
		fdc->sr2 &= ~0x08;	// tmp!=data : SH=0 for all
	switch(fdc->com & 0x0c) {
		case 0x00:
			if (fdc->tmp != fdc->data)
				fdc->sr2 |= 0x04;	// SN=1 : not meet
			break;
		case 0x08:
			if (fdc->tmp > fdc->data)
				fdc->sr2 |= 0x04;
			break;
		case 0x0c:
			if (fdc->tmp < fdc->data)
				fdc->sr2 |= 0x04;
			break;
	}
	fdc->cnt--;
	if (fdc->cnt < 1) {
		fdc->pos++;
		fdc->wait = fdc->bytedelay; // turbo ? TRBBYTE : fdc->bytedelay;
	}
}

// check end
void uscan03(FDC* fdc) {
	if ((fdc->sr2 & 0x08) || (fdc->sr2 & 0x04)) {	// scan succes OR deleted sector found
		fdc->pos++;
	} else {
		fdc->sec += fdc->comBuf[7];		// STP
		if (fdc->sec >= fdc->comBuf[5]) {	// check EOT
			fdc->pos++;
		} else {
			fdc->cnt = 2;
			fdc->pos = 2;
		}
	}
}

static fdcCall uScan[] = {&uwargs, &uread00, &uread01, &uscan00, &uscan01, &uscan02, &uscan03, &ureadRS, &uTerm};

// MT.MF.SK.00101 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,DTL] R [SR0,SR1,SR2,C,H,R,N]
// MT.MF.SK.01001 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,DTL] R [SR0,SR1,SR2,C,H,R,N]
// write [deleted] data

void uwrdat00(FDC* fdc) {
	uSetDrive(fdc);
	fdc->sec = fdc->comBuf[3];		// R, sec.num
	if (!(fdc->flp->insert && fdc->flp->door)) {
		fdc->sr0 |= 0x48;		// not ready
		ureadRS(fdc);
		uTerm(fdc);
	} else if (fdc->flp->protect)	{
		fdc->sr1 |= F_SR1_NW;
		fdc->sr0 |= 0x40;
		ureadRS(fdc);
		uTerm(fdc);
	} else {
		fdc->cnt = 2;
		fdc->pos++;
		fdc->drq = 1;
		fdc->dir = 0;
	}
}

// wait sector header
void uwrdat01(FDC* fdc) {
	int res = seekADR(fdc);
	if (res == 2) {				// index
		fdc->cnt--;
		if (fdc->cnt < 1) {
			fdc->sr0 |= 0x40;	// error
			fdc->sr1 |= F_SR1_ND;	// no data
			ureadRS(fdc);
			uTerm(fdc);
		}
	} else if (res == 1) {			// found
		if (fdc->sec == fdc->buf[2]) {	// same sector
			fdc->cnt = 52;
			fdc->pos++;
		}
	}
}

// wait sector data
void uwrdat02(FDC* fdc) {
	flpNext(fdc->flp, fdc->side);
	if ((fdc->flp->field == 2) || (fdc->flp->field == 3)) {		// data filed (excluding A1 A1 A1 F8(FB))
		flpPrev(fdc->flp, fdc->side);				// to data mark (F8 | FB)
		fdc->crc = 0;						// form crc for A1 A1 A1
		add_crc_16(fdc, 0xa1);
		add_crc_16(fdc, 0xa1);
		add_crc_16(fdc, 0xa1);
		unsigned char mark = (fdc->com & 8) ? 0xf8 : 0xfb;
		flpWr(fdc->flp, fdc->side, mark);
		add_crc_16(fdc, mark);
		flpNext(fdc->flp, fdc->side);
		fdc->wait = fdc->bytedelay;
		// todo : data length
		fdc->cnt = 128 << (fdc->buf[3] & 7);
		// printf("cnt = %i\n", fdc->cnt);
		fdc->tns = 0;
		fdc->pos++;
	} else {
		fdc->wait += turbo ? TRBBYTE : fdc->bytedelay;
		fdc->cnt--;
		if (fdc->cnt < 1) {
			fdc->sr0 |= 0x40;		// error
			fdc->sr1 |= F_SR1_ND;		// no data
			ureadRS(fdc);
			uTerm(fdc);
		}
	}
}

// write data

void uwrdat03(FDC* fdc) {
	if (fdc->mr) {
		flpWr(fdc->flp, fdc->side, 0x00);
	} else {
		if (fdc->drq) {				// still wait byte from cpu
			fdc->sr1 |= F_SR1_OR;		// overrun. don't terminate command
		}
		flpWr(fdc->flp, fdc->side, fdc->data);
	}
	flpNext(fdc->flp, fdc->side);
	fdc->wait += fdc->bytedelay;
	fdc->cnt--;
	if (fdc->cnt < 1) {
		fdc->pos++;
	} else {
		fdc->drq = 1;		// request next byte
		if (fdc->dma) {
			fdc->xirq(IRQ_FDC_WR, fdc->xptr);
		} else {
			uInt(fdc);
//			fdc->intr = 1;
//			if (fdc->inten) fdc->xirq(IRQ_FDC, fdc->xptr);
		}
	}
}

// write crc
void uwrdat04(FDC* fdc) {
	flpWr(fdc->flp, fdc->side, (fdc->crc >> 8) & 0xff);
	flpNext(fdc->flp, fdc->side);
	fdc->wait += fdc->bytedelay;
	flpWr(fdc->flp, fdc->side, fdc->crc & 0xff);
	flpNext(fdc->flp, fdc->side);
	fdc->wait += fdc->bytedelay;
	fdc->pos++;
}

// check eot/mt
void uwrdat05(FDC* fdc) {
	fdc->sec++;
	if (fdc->mr) {					// TC
		fdc->pos++;
	} else if (fdc->sec > fdc->comBuf[5]) {		// check EOT
		if ((fdc->com & F_COM_MT) && !fdc->side) {		// multitrack (side 0->1 only)
			fdc->side = 1;
			fdc->cnt = 2;
			fdc->sec = 1;
			fdc->pos = 2;	// to uwrdat01
			fdc->drq = 1;
			fdc->dir = 0;
		} else {
			fdc->pos++;
		}
	} else {
		fdc->pos = 2;	// to uwrdat01
		fdc->drq = 1;
		fdc->dir = 0;
	}
}

static fdcCall uWrData[] = {&uwargs,&uwrdat00,&uwrdat01,&uwrdat02,&uwrdat03,&uwrdat04,&uwrdat05,&ureadRS,&uTerm};

// 0.MF.001101 [xxxxx.H.DRV:1, N, SC, GPL, D] R [SR0,SR1,SR2,C,H,R,N]
// 1:N:bytes/sector
// 2:SC:sectors/trk
// 3:GPL:gap3 size
// 4:D:filler byte
// fdc wants C,H,R,N for each sector.
// writing start @ index, stop @ next index

void utrkfrm00(FDC* fdc) {
	uSetDrive(fdc);
	if (!(fdc->flp->insert && fdc->flp->door)) {
		fdc->sr0 |= 0x48;		// not ready
		ureadRS(fdc);
		uTerm(fdc);
	} else if (fdc->flp->protect) {
		fdc->sr0 |= 0x40;
		fdc->sr1 |= F_SR1_NW;
		ureadRS(fdc);
		uTerm(fdc);
	} else {
		fdc->pos++;
	}
}

void utrkfrm01(FDC* fdc) {		// wait for index
	flpNext(fdc->flp, fdc->side);
	fdc->wait += fdc->bytedelay; // turbo ? TRBBYTE : fdc->bytedelay;
	if (fdc->flp->index) {
		DBGOUT("index\n");
		fdc->cnt = 0;
		fdc->scnt = 0;
		fdc->dir = 0;
		fdc->drq = 1;		// waiting for 1st C
		if (fdc->dma) {
			fdc->xirq(IRQ_FDC_WR, fdc->xptr);
		} else {
			uInt(fdc);
//			fdc->intr = 1;	// generate interrupt
//			if (fdc->inten) fdc->xirq(IRQ_FDC, fdc->xptr);
		}
		fdc->pos++;
	}
}

void diskFormTrack(Floppy* flp, int tr, Sector* sdata, int scount);

void utrkfrm02(FDC* fdc) {		// recieve cnt (4 * spt) bytes from cpu = sectors header data
	if (!fdc->drq) {		// byte recieved
		fdc->buf[fdc->cnt & 3] = fdc->data;
		fdc->cnt++;
		fdc->drq = 1;			// want next byte
		if (fdc->cnt >= 4) {		// if all 4 bytes is recieved, register new sector
			fdc->cnt = 0;
			fdc->slst[fdc->scnt].trk = fdc->buf[0];		// C cylinder
			fdc->slst[fdc->scnt].head = fdc->buf[1];	// H head
			fdc->slst[fdc->scnt].sec = fdc->buf[2];		// R sec.num
			fdc->slst[fdc->scnt].sz = fdc->buf[3];		// N sec.size
			fdc->slst[fdc->scnt].crc = -1;			// to be calculated
			fdc->slst[fdc->scnt].type = 0xfb;		// type: normal
			memset(fdc->slst[fdc->scnt].data, fdc->comBuf[4], 4096);	// fill sector data
			fdc->scnt++;					// next sector
			if (fdc->scnt >= fdc->comBuf[2]) {		// sectors/track
				diskFormTrack(fdc->flp, (fdc->trk << 1) | fdc->side, fdc->slst, fdc->comBuf[2]);
				fdc->drq = 0;				// all data completed, no more bytes needed
				fdc->pos++;
			} else {
				fdc->drq = 1;
				if (fdc->dma) {
					fdc->xirq(IRQ_FDC_WR, fdc->xptr);
				} else {
					uInt(fdc);
					//fdc->intr = 1;
					//if (fdc->inten) fdc->xirq(IRQ_FDC, fdc->xptr);
				}
			}
		} else {
			fdc->drq = 1;		// else request new byte
			if (fdc->dma) {
				fdc->xirq(IRQ_FDC_WR, fdc->xptr);
			} else {
				uInt(fdc);
				//fdc->intr = 1;
				//if (fdc->inten) fdc->xirq(IRQ_FDC, fdc->xptr);
			}
		}
	}
	fdc->wait += fdc->bytedelay;
}

void utrkfrm03(FDC* fdc) {		// emulate whole track writing
#if 0
	// immediate next step
	fdc->pos++;
#else
	// wait for next index
	if (flpNext(fdc->flp, fdc->side)) {
		fdc->pos++;
	} else {
		fdc->wait += fdc->bytedelay;
	}
#endif
}

static fdcCall uFormat[] = {&uwargs,&utrkfrm00,&utrkfrm01,&utrkfrm02,&utrkfrm03,&ureadRS,&uTerm};

// invalid op : R [SR0 = 0x80]

void uinv00(FDC* fdc) {
	fdc->sr0 = 0x80;
	fdc->resBuf[0] = fdc->sr0;
	uResp(fdc, 1, 0);
	fdc->pos++;
}

static fdcCall uInvalid[] = {&uwargs,&uinv00,&uTerm};

// 00001000 R [ST0],[PCN]
// sense interrupt status

// Idea: this com works properly only after seek/recalibrate coms?
void usint00(FDC* fdc) {
	fdc->intr = 0;
	if (!fdc->seekend && fdc->upd) {
		fdc->plan = uInvalid;
		fdc->pos = 0;
	} else {
		fdc->state &= 0xf0;		// clear b0..3 of main status register
		fdc->resBuf[0] = fdc->sr0;
		fdc->resBuf[1] = fdc->trk;
		uResp(fdc, 2, 0);		// no int on response/reset interrupt
		fdc->seekend = 0;
		fdc->pos++;
	}
}

static fdcCall uSenseInt[] = {&uwargs,&usint00,&uTerm};

// common

static uCom uComTab[] = {
	{0x1f, 0x06, 8, uRdData},	// xxx00110 read data
	{0x1f, 0x0c, 8, uRdData},	// xxx01100 read deleted data
	{0x3f, 0x05, 8, uWrData},	// xx000101 write data
	{0x3f, 0x09, 8, uWrData},	// xx001001 write deleted data
	{0x1f, 0x02, 8, uRdTrk},	// -xx00010 read track		TODO:check
	{0xbf, 0x0a, 1, uReadID},	// 0x001010 read ID
	{0x3f, 0x0d, 5, uFormat},	// -x001101 format track	ibm send CD for format trk
	{0x1f, 0x11, 8, uScan},		// xxx10001 scan equal		TODO:check scan commands
	{0x1f, 0x19, 8, uScan},		// xxx11001 scan low or equal
	{0x1f, 0x1d, 8, uScan},		// xxx11101 scan high or equal
	{0xff, 0x07, 1, uCalib},	// 00000111 recalibrate
	{0xff, 0x08, 0, uSenseInt},	// 00001000 sense interrupt status
	{0xff, 0x03, 2, uSpecify},	// 00000011 specify
	{0xff, 0x04, 1, uDrvStat},	// 00000100 sense drive status
	{0xff, 0x0f, 2, uSeek},		// 00001111 seek
	{0x00, 0x00, 0, uInvalid}	// -------- invalid op
};

// 1)'sense interrupt status' com must be sent after 'seek' and 'recalibrate'; otherwise, fdc will consider next com as invalid
// 2)issuing 'sense interrupt status' com without interrupt pending is treated as invalid com
// 3)'seek' and 'recalibrate' doesn't have result phase, 'sense interrupt status' must be used to effectively terminate them

void uExec(FDC* fdc, unsigned char val) {
	fdc->com = val;
	int idx = 0;
	while ((uComTab[idx].mask & val) != uComTab[idx].val)
		idx++;
	fdc->comCnt = uComTab[idx].argCount;
	fdc->plan = uComTab[idx].plan;
	fdc->pos = 0;
	fdc->comPos = 0;
	fdc->idle = 0;
	if (fdc->comCnt > 0)
		fdc->drq = 1;	// wait for args
	fdc->dir = 0;		// cpu->fdc
	fdc->wait = 0;
}

void uWrite(FDC* fdc, int adr, unsigned char val) {
	if (!(adr & 1)) return;			// wr data only
	// printf("wr : %.2X\n", val);
	fdc->intr = 0;				// reset interrupt
	if (fdc->idle) {			// 1st byte, command
		// printf("updCom %.2X\n",val);
		if (fdc->seekend) {		// 'sense interrupt status' after seek/recalibrate only
			uExec(fdc, (val == 0x08) ? val : 0xff);
		} else {			// TODO: 'sense interrupt status' here is illegal
			uExec(fdc, val);
			if (val != 0x08) {
				fdc->sr0 = fdc->flp->id & 3;
				fdc->sr1 = 0x00;
				fdc->sr2 = 0x00;
			}
		}
	} else if (fdc->comCnt > 0) {		// arguments
		// printf("arg %.2X\n",val);
		fdc->comBuf[fdc->comPos] = val;
		fdc->comPos++;
		fdc->comCnt--;
		if (fdc->comCnt == 0) {
			fdc->irq = 1;		// exec
			fdc->drq = 0;
			// printf("FDC com: %.2X ",fdc->com); for (int i = 0; i < fdc->comPos; i++) {printf("%.2X ", fdc->comBuf[i]);} printf("\n");	// print command buf
		}
	} else if (fdc->drq && !fdc->dir) {	// data cpu->fdc
		// printf("data %.2X\n",val);
		fdc->data = val;
		fdc->drq = 0;
	} else if (val == 0x04) {
		printf("sense drive status\n");
	}
}

unsigned char uRead(FDC* fdc, int adr) {
	unsigned char res = 0xff;
	fdc->intr = 0;					// reset interrupt
	if (adr & 1) {					// 1: data
		if (fdc->drq && fdc->dir) {		// data fdc->cpu
			if (fdc->irq) {			// execution: transfer data
				res = fdc->data;
				fdc->drq = 0;
			} else if (fdc->resCnt > 0) {	// result phase
				fdc->intr = 0;		// reset interrupt @ reading [TODO: on 1st byte of result]
				res = fdc->resBuf[fdc->resPos];
				fdc->resPos++;
				fdc->resCnt--;
				if (fdc->resCnt == 0) {	// result phase completed
					fdc->dir = 0;	// cpu->fdc, waiting for command
					fdc->idle = 1;
				}
			} else {			// other
				res = 0xff;
			}
		}
	} else {			// RD 0 : main status register
		if (fdc->idle) {
			res = 0x80 | ((fdc->resCnt > 0) ? 0x40 : 0x00);
		} else {
			res = (fdc->state & 0x1f) | (fdc->drq << 7) | (fdc->dir << 6);
			if (fdc->irq && !fdc->dma)		// only in non-dma mode
				res |= 0x20;
		}
	}
	return res;
}

// terminal count (TC pin)
void uTCount(FDC* fdc) {
	fdc->mr = 1;		// acts as Terminal Count: break current read/write operation
}

void uReset(FDC* fdc) {
	fdc->idle = 1;
	fdc->drq = 1;
	fdc->dir = 1;
	fdc->mr = 0;
	fdc->plan = NULL;
	fdc->wait = 0;
	fdc->trk = 0;
	fdc->sec = 0;
	fdc->data = 0;
	fdc->state = 0;
	fdc->srt = 1000;
	fdc->hlt = 0;
	fdc->hut = 0;
	fdc->resCnt = 0;
	fdc->resPos = 0;
	// TODO:check this
	fdc->intr = 0;		// ibm bios want interrupt after reset
	uInt(fdc);
	//fdc->xirq(IRQ_FDC, fdc->xptr);
	fdc->sr0 = 0xc0;	// and b7,6=11 in sr0
}
