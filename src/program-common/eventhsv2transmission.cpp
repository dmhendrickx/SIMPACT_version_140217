#include "eventhsv2transmission.h"
#include "jsonconfig.h"
#include "configfunctions.h"
#include "util.h"
#include <cmath>
#include <iostream>

using namespace std;

// Conception happens between two people, so using this constructor seems natural.
// Also, when one of the involved persons dies before this is fired, the event is
// removed automatically.
EventHSV2Transmission::EventHSV2Transmission(Person *pPerson1, Person *pPerson2) : SimpactEvent(pPerson1, pPerson2)
{
	// is about transmission from pPerson1 to pPerson2, so no ordering according to
	// gender here
	assert(pPerson1->hsv2().isInfected() && !pPerson2->hsv2().isInfected());
}

EventHSV2Transmission::~EventHSV2Transmission()
{
}

string EventHSV2Transmission::getDescription(double tNow) const
{
	return strprintf("HSV2 Transmission event from %s to %s", getPerson(0)->getName().c_str(), getPerson(1)->getName().c_str());
}

void EventHSV2Transmission::writeLogs(const SimpactPopulation &pop, double tNow) const
{
	Person *pPerson1 = getPerson(0);
	Person *pPerson2 = getPerson(1);
	writeEventLogStart(true, "HSV2 transmission", tNow, pPerson1, pPerson2);
}

// The dissolution event that makes this event useless involves the exact same people,
// so this function will automatically make sure that this conception event is discarded
// (this function is definitely called for those people)

bool EventHSV2Transmission::isUseless(const PopulationStateInterface &population)
{
	// Transmission from pPerson1 to pPerson2
	Person *pPerson1 = getPerson(0);
	Person *pPerson2 = getPerson(1);

	// If person2 already became HSV2 positive, there no sense in further transmission
	if (pPerson2->hsv2().isInfected())
		return true;

	// Event is useless if the relationship between the two people is over
	if (!pPerson1->hasRelationshipWith(pPerson2))
	{
		assert(!pPerson2->hasRelationshipWith(pPerson1));
		return true;
	}

	// Make sure the two lists are consistent: if person1 has a relationship with person2, person2
	// should also have a relationship with person1
	assert(pPerson2->hasRelationshipWith(pPerson1));

	return false;
}

void EventHSV2Transmission::infectPerson(SimpactPopulation &population, Person *pOrigin, Person *pTarget, double t)
{
	assert(!pTarget->hsv2().isInfected());

	if (pOrigin == 0) // Seeding
		pTarget->hsv2().setInfected(t, 0, Person_HSV2::Seed);
	else
	{
		assert(pOrigin->hsv2().isInfected());
		pTarget->hsv2().setInfected(t, pOrigin, Person_HSV2::Partner);
	}

	// Check relationships pTarget is in, and if the partner is not yet infected, schedule
	// a transmission event.
	int numRelations = pTarget->getNumberOfRelationships();
	pTarget->startRelationshipIteration();
	
	for (int i = 0 ; i < numRelations ; i++)
	{
		double formationTime = -1;
		Person *pPartner = pTarget->getNextRelationshipPartner(formationTime);

		if (!pPartner->hsv2().isInfected())
		{
			EventHSV2Transmission *pEvtTrans = new EventHSV2Transmission(pTarget, pPartner);
			population.onNewEvent(pEvtTrans);
		}
	}

#ifndef NDEBUG
	double tDummy;
	assert(pTarget->getNextRelationshipPartner(tDummy) == 0);
#endif // NDEBUG
}

void EventHSV2Transmission::fire(Algorithm *pAlgorithm, State *pState, double t)
{
	SimpactPopulation &population = SIMPACTPOPULATION(pState);
	// Transmission from pPerson1 to pPerson2
	Person *pPerson1 = getPerson(0);
	Person *pPerson2 = getPerson(1);

	// Person 1 should be infected , person 2 should not be infected yet
	assert(pPerson1->hsv2().isInfected());
	assert(!pPerson2->hsv2().isInfected());
	
	infectPerson(population, pPerson1, pPerson2, t);
}

double EventHSV2Transmission::calculateInternalTimeInterval(const State *pState, double t0, double dt)
{
	Person *pOrigin = getPerson(0);
	Person *pTarget = getPerson(1);
	double tMax = getTMax(pOrigin, pTarget);

	HazardFunctionHSV2Transmission h0(pOrigin, pTarget);
	TimeLimitedHazardFunction h(h0, tMax);

	return h.calculateInternalTimeInterval(t0, dt);
}

double EventHSV2Transmission::solveForRealTimeInterval(const State *pState, double Tdiff, double t0)
{
	Person *pOrigin = getPerson(0);
	Person *pTarget = getPerson(1);
	double tMax = getTMax(pOrigin, pTarget);

	HazardFunctionHSV2Transmission h0(pOrigin, pTarget);
	TimeLimitedHazardFunction h(h0, tMax);

	return h.solveForRealTimeInterval(t0, Tdiff);
}

double EventHSV2Transmission::s_tMax = 200;
double EventHSV2Transmission::HazardFunctionHSV2Transmission::s_b = 0;

void EventHSV2Transmission::processConfig(ConfigSettings &config, GslRandomNumberGenerator *pRndGen)
{
	bool_t r;

    if (!(r = config.getKeyValue("hsv2transmission.hazard.b", HazardFunctionHSV2Transmission::s_b)) ||
        !(r = config.getKeyValue("hsv2transmission.hazard.t_max", s_tMax))
        )
        abortWithMessage(r.getErrorString());

}

void EventHSV2Transmission::obtainConfig(ConfigWriter &config)
{
	bool_t r;

	if (!(r = config.addKey("hsv2transmission.hazard.b", HazardFunctionHSV2Transmission::s_b)) ||
		!(r = config.addKey("hsv2transmission.hazard.t_max", s_tMax))
		)
		abortWithMessage(r.getErrorString());
}

double EventHSV2Transmission::getTMax(const Person *pPerson1, const Person *pPerson2)
{
    assert(pPerson1 != 0 && pPerson2 != 0);

    double tb1 = pPerson1->getDateOfBirth();
    double tb2 = pPerson2->getDateOfBirth();

    double tMax = tb1;

    if (tb2 < tMax)
        tMax = tb2;

    assert(s_tMax > 0);
    tMax += s_tMax;
    return tMax;
}

EventHSV2Transmission::HazardFunctionHSV2Transmission::HazardFunctionHSV2Transmission(const Person *pPerson1, 
                                                                                      const Person *pPerson2)
    : HazardFunctionExp(getA(pPerson1, pPerson2), s_b)
{
}

EventHSV2Transmission::HazardFunctionHSV2Transmission::~HazardFunctionHSV2Transmission()
{
}

double EventHSV2Transmission::HazardFunctionHSV2Transmission::getA(const Person *pOrigin, const Person *pTarget)
{
    assert(pOrigin);
    return pOrigin->hsv2().getHazardAParameter() - s_b*pOrigin->hsv2().getInfectionTime();
}

ConfigFunctions hsv2TransmissionConfigFunctions(EventHSV2Transmission::processConfig, EventHSV2Transmission::obtainConfig, 
		                                        "EventHSV2Transmission");

JSONConfig hsv2TransmissionJSONConfig(R"JSON(
        "EventHSV2Transmission": { 
            "depends": null,
            "params": [ 
				[ "hsv2transmission.hazard.b", 0 ],
				[ "hsv2transmission.hazard.t_max", 200 ]
			],
            "info": [ 
				"These configuration parameters allow you to set the 'b' value in the hazard",
				" h = exp(a_i + b*(t-t_infected))",
				"The value of 'a_i' depends on the individual, and can be specified as a ",
				"distribution in the person parameters."
            ]
        })JSON");

