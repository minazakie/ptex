/* 
   CONFIDENTIAL INFORMATION: This software is the confidential and
   proprietary information of Walt Disney Animation Studios ("Disney").
   This software is owned by Disney and may not be used, disclosed,
   reproduced or distributed for any purpose without prior written
   authorization and license from Disney. Reproduction of any section of
   this software must include this legend and all copyright notices.
   (c) Disney. All rights reserved.
*/

#include "PtexPlatform.h"
#include <math.h>
#include <assert.h>

#include "PtexTriangleFilter.h"
#include "PtexTriangleKernel.h"
#include "PtexUtils.h"

namespace {
    inline double squared(double x) { return x*x; }
}

void PtexTriangleFilter::eval(float* result, int firstChan, int nChannels,
			      int faceid, float u, float v,
			      float uw1, float vw1, float uw2, float vw2,
			      float width, float blur)
{
    // init
    if (!_tx || nChannels <= 0) return;
    if (faceid < 0 || faceid >= _tx->numFaces()) return;
    _ntxchan = _tx->numChannels();
    _dt = _tx->dataType();
    _firstChanOffset = firstChan*DataSize(_dt);
    _nchan = PtexUtils::min(nChannels, _ntxchan-firstChan);

    // get face info
    const FaceInfo& f = _tx->getFaceInfo(faceid);

    // if neighborhood is constant, just return constant value of face
    if (f.isNeighborhoodConstant()) {
	PtexPtr<PtexFaceData> data ( _tx->getData(faceid, 0) );
	if (data) {
	    char* d = (char*) data->getData() + _firstChanOffset;
	    Ptex::ConvertToFloat(result, d, _dt, _nchan);
	}
	return;
    }
    
    // clamp u and v
    u = PtexUtils::clamp(u, 0.0f, 1.0f);
    v = PtexUtils::clamp(v, 0.0f, 1.0f);

    // build kernel
    PtexTriangleKernel k;
    buildKernel(k, u, v, uw1, vw1, uw2, vw2, width, blur, f.res);

    // accumulate the weight as we apply
    _weight = 0;

    // allocate temporary double-precision result
    _result = (double*) alloca(sizeof(double)*_nchan);
    memset(_result, 0, sizeof(double)*_nchan);

    // apply to faces
    splitAndApply(k, faceid, f);

    // normalize (both for data type and cumulative kernel weight applied)
    // and output result
    double scale = 1.0 / (_weight * OneValue(_dt));
    for (int i = 0; i < _nchan; i++) result[i] = float(_result[i] * scale);

    // clear temp result
    _result = 0;
}



void PtexTriangleFilter::buildKernel(PtexTriangleKernel& k, float u, float v, 
					float uw1, float vw1, float uw2, float vw2,
					float width, float blur, Res faceRes)
{
    // compute ellipse coefficients, A*u^2 + B*u*v + C*v^2 == AC - B^2/4
    double scale = width*width;
    double A = (vw1*vw1 + vw2*vw2) * scale * 0.25;
    double B = -(uw1*vw1 + uw2*vw2) * scale * 0.5;
    double C = (uw1*vw2 - vw1*uw2) * scale * 0.25;

    // convert to cartesian domain
    double Ac = 0.75 * A;
    double Bc = 0.8660254037844386 * (B-A); // 0.866 = sqrt(3/4.)
    double Cc = 0.25 * A - 0.5 * B + C;

#if 0
    if (Ac*Cc - Bc*Bc*.25 <= 0) {
	// degenerate ellipse, 
	
    }
#endif

    // compute min blur for eccentricity clamping
    // TODO - tune maxEcc setting
    const double maxEcc = 10; // max eccentricity
    const double eccRatio = (maxEcc*maxEcc + 1) / (maxEcc*maxEcc - 1);
    double X = sqrt(squared(Ac - Cc) + squared(Bc));
    double b_e = 0.5 * (eccRatio * X - (Ac + Cc));

    // compute min blur for texel clamping
    // (ensure that ellipse is no smaller than a texel)
    double b_t = squared(0.5 / faceRes.u());

    // add blur
    double b_b = 0.25 * blur * blur;
    double b = PtexUtils::max(b_b, PtexUtils::max(b_e, b_t));
    Ac += b;
    Cc += b;

    // compute minor radius
    double m = sqrt(2*(Ac*Cc - 0.25*Bc*Bc) / (Ac + Cc + X));
    
    // choose resolution, clamp against face resolution
    int reslog2 = PtexUtils::min(int(ceil(log2(0.5/m))),
				 int(faceRes.ulog2));

    // scale
    Ac *= 2.5;
    Cc *= 2.5;

    // convert back to triangular domain
    A = (4/3.0) * Ac;
    B = 1.1547005383792515 * Bc + A; // 1.15 = sqrt(4/3.)
    C = -0.25 * A + 0.5 * B + Cc;

    // find u,v,w extents
    // TODO - determine best kernel scale factor
    double uw = sqrt(C);
    double vw = sqrt(A);
    double ww = sqrt(A-B+C);

    // init kernel
    double w = 1 - u - v;
    k.set(Res(reslog2, reslog2), u, v, u-uw, v-vw, w-ww, u+uw, v+vw, w+ww, A, B, C);
}


void PtexTriangleFilter::splitAndApply(PtexTriangleKernel& k, int faceid, const Ptex::FaceInfo& f)
{
    // do we need to split? if so, split kernel and apply across edge(s)
    if (k.u1 < 0 && f.adjface(2)) {
	PtexTriangleKernel ka;
	k.splitU(ka);
	applyAcrossEdge(ka, f, 2);
    }
    if (k.v1 < 0 && f.adjface(0)) {
	PtexTriangleKernel ka;
	k.splitV(ka);
	applyAcrossEdge(ka, f, 0);
    }
    if (k.w1 < 0 && f.adjface(1)) {
	PtexTriangleKernel ka;
	k.splitW(ka);
	applyAcrossEdge(ka, f, 1);
    }

    // apply to local face
    apply(k, faceid, f); 
}


void PtexTriangleFilter::applyAcrossEdge(PtexTriangleKernel& k, 
					 const Ptex::FaceInfo& f, int eid)
{
    // TODO - finish this once splitting is working - implement reorient
    return;

    int afid = f.adjface(eid), aeid = f.adjedge(eid);
    const Ptex::FaceInfo& af = _tx->getFaceInfo(afid);
    k.reorient(eid, aeid, af.res);
    apply(k, afid, af);
}


void PtexTriangleFilter::apply(PtexTriangleKernel& k, int faceid, const Ptex::FaceInfo& f)
{
    // clamp kernel res to face res
    k.clampRes(f.res);

    // build kernel iterators
    PtexTriangleKernelIter keven, kodd;
    k.getIterators(keven, kodd);
    if (!keven.valid && !kodd.valid) return;

    // get face data, and apply
    PtexPtr<PtexFaceData> dh ( _tx->getData(faceid, k.res) );
    if (!dh) return;

    if (keven.valid) applyIter(keven, dh);
        if (kodd.valid) applyIter(kodd, dh);
}


void PtexTriangleFilter::applyIter(PtexTriangleKernelIter& k, PtexFaceData* dh)
{
    if (dh->isConstant()) {
	k.applyConst(_result, (char*)dh->getData()+_firstChanOffset, _dt, _nchan);
	_weight += k.weight;
    }
    else if (dh->isTiled()) {
#if 0
	// TODO implement tile iterator for triangle kernel
	Ptex::Res tileres = dh->tileRes();
	PtexTriangleKernel kt;
	kt.res = tileres;
	int tileresu = tileres.u();
	int tileresv = tileres.v();
	int ntilesu = k.res.u() / tileresu;
	for (int v = k.v, vw = k.vw; vw > 0; vw -= kt.vw, v += kt.vw) {
	    int tilev = v / tileresv;
	    kt.v = v % tileresv;
	    kt.vw = PtexUtils::min(vw, tileresv - kt.v);
	    kt.kv = k.kv + v - k.v;
	    for (int u = k.u, uw = k.uw; uw > 0; uw -= kt.uw, u += kt.uw) {
		int tileu = u / tileresu;
		kt.u = u % tileresu;
		kt.uw = PtexUtils::min(uw, tileresu - kt.u);
		kt.ku = k.ku + u - k.u;
		PtexPtr<PtexFaceData> th ( dh->getTile(tilev * ntilesu + tileu) );
		if (th) {
		    if (th->isConstant())
			kt.applyConst(_result, (char*)th->getData()+_firstChanOffset, _dt, _nchan);
		    else
			kt.apply(_result, (char*)th->getData()+_firstChanOffset, _dt, _nchan, _ntxchan);
		}
	    }
	}
#endif
    }
    else {
	k.apply(_result, (char*)dh->getData()+_firstChanOffset, _dt, _nchan, _ntxchan);
	_weight += k.weight;
    }
}