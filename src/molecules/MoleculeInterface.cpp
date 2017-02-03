/*
 * MoleculeInterface.cpp
 *
 *  Created on: 21 Jan 2017
 *      Author: tchipevn
 */

#include "MoleculeInterface.h"

#include <cassert>
#include <cmath>
#include <fstream>

#include "utils/Logger.h"

using namespace std;
using Log::global_log;


MoleculeInterface::~MoleculeInterface() {
	// TODO Auto-generated destructor stub
}

bool MoleculeInterface::isLessThan(const MoleculeInterface& m2) const {
	if (r(2) < m2.r(2))
		return true;
	else if (r(2) > m2.r(2))
		return false;
	else {
		if (r(1) < m2.r(1))
			return true;
		else if (r(1) > m2.r(1))
			return false;
		else {
			if (r(0) < m2.r(0))
				return true;
			else if (r(0) > m2.r(0))
				return false;
			else {
				global_log->error() << "LinkedCells::isFirstParticle: both Particles have the same position" << endl;
				exit(1);
			}
		}
	}
	return false; /* Silence warnings about missing return statement */
}
