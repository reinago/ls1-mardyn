/*
 * COMaligner.h
 *
 *  Created on: 7 May 2018
 *      Author: kruegener
 */

#ifndef MARDYN_TRUNK_COMALIGNER_H
#define MARDYN_TRUNK_COMALIGNER_H

class COMalignerTest;
#include "PluginBase.h"
#include "particleContainer/ParticleContainer.h"
#include "Domain.h"
#include "parallel/DomainDecompBase.h"

/** @brief
* Plugin: can be enabled via config.xml <br>
*
* Calculates Center of mass and moves all particles to align with center of box
* Individual dimensions X,Y,Z can be toogled on/off for the alignment
* Alignment happens once every interval-simsteps
* The correction factor can be set from 0-1
* 1 being full alignment -> 0 no alignment at all
* \code{.xml}
* <plugin name="COMaligner">
*			<x>1</x>
*			<y>0</y>
*			<z>1</z>
*			<interval>1</interval>
*			<correctionFactor>.5</correctionFactor>
*	</plugin>
* \endcode
*/
class COMaligner : public PluginBase{

private:
    friend COMalignerTest;
    // DEFAULT: ALIGN IN ALL DIMENSIONS
    bool _alignX = true;
    bool _alignY = true;
    bool _alignZ = true;

    bool _enabled = true;

    int _dim_start = 0;
    int _dim_end = 3;
    int _dim_step = 1;

    // DEFAULT: EVERY FRAME FULL ALIGNMENT
    int _interval = 1;
    float _alignmentCorrection = 1.0f;

    double _motion[3];
    double _balance[3];
    double _mass = 0.0;
    double _boxLength[3];
    double _cutoff;

public:
    COMaligner(){
        // SETUP
        for (unsigned d = 0; d < 3; d++) {
            _balance[d] = 0.0;
            _motion[d] = 0.0;
        }
    };
    ~COMaligner(){};

    void init(ParticleContainer* particleContainer, DomainDecompBase* domainDecomp, Domain* domain) override {
        global_log -> debug() << "COM Realignment enabled" << std::endl;

        for(unsigned d = 0; d < 3; d++){
            _boxLength[d] = domain->getGlobalLength(d);
        }

        _cutoff = .9*particleContainer->getCutoff();

    }

    void readXML (XMLfileUnits& xmlconfig) override;

    void beforeForces(ParticleContainer* particleContainer, DomainDecompBase* domainDecomp, unsigned long simstep) override;

    void endStep(
            ParticleContainer *particleContainer, DomainDecompBase *domainDecomp,
            Domain *domain, unsigned long simstep);


    void finish(ParticleContainer *particleContainer,
                DomainDecompBase *domainDecomp, Domain *domain) override {};

    std::string getPluginName()override {return std::string("COMaligner");}

    static PluginBase* createInstance(){return new COMaligner();}

};


#endif //MARDYN_TRUNK_COMALIGNER_H