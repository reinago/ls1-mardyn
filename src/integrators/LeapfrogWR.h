/*
 * LeapfrogWR.h
 *
 *  Created on: Apr 16, 2017
 *      Author: tchipevn
 */

#ifndef SRC_INTEGRATORS_LEAPFROGWR_H_
#define SRC_INTEGRATORS_LEAPFROGWR_H_

#include "Integrator.h"

class Leapfrog_WR : public Integrator {
public:
	Leapfrog_WR() {}
	Leapfrog_WR(double timestepLength);
	~Leapfrog_WR() {}

	void readXML(XMLfileUnits& xmlconfig);

	void init() {}

	void eventForcesCalculated(ParticleContainer* moleculeContainer, Domain* domain) {
		computeVelocities(moleculeContainer, domain);
	}

	void eventNewTimestep(ParticleContainer* moleculeContainer, Domain* domain) {
		computePositions(moleculeContainer, domain);
	}

	void accelerateUniformly(
			ParticleContainer* molCont,
			Domain* domain
	) {}
	void accelerateInstantaneously(
			ParticleContainer* molCont,
			Domain* domain
	) {}

private:
	
	void computePositions(ParticleContainer* molCont, Domain* dom);
	void computeVelocities(ParticleContainer* molCont, Domain* dom);

};

#endif /* SRC_INTEGRATORS_LEAPFROGWR_H_ */