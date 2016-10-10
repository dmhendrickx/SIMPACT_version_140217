#include "eventmonitoring.h"
#include "configsettings.h"
#include "configwriter.h"
#include "eventdropout.h"
#include "configdistributionhelper.h"
#include "gslrandomnumbergenerator.h"
#include "piecewiselinearfunction.h"
#include "point2d.h"
#include "jsonconfig.h"
#include "configfunctions.h"
#include "util.h"
#include "facilities.h"
#include "maxartpopulation.h"
#include <iostream>

using namespace std;

EventMonitoring::EventMonitoring(Person *pPerson, bool scheduleImmediately) : SimpactEvent(pPerson)
{
	m_scheduleImmediately = scheduleImmediately;
}

EventMonitoring::~EventMonitoring()
{
}

string EventMonitoring::getDescription(double tNow)
{
	return strprintf("Monitoring event for %s", getPerson(0)->getName().c_str());
}

void EventMonitoring::writeLogs(const Population &pop, double tNow) const
{
	Person *pPerson = getPerson(0);
	writeEventLogStart(false, "monitoring", tNow, pPerson, 0);

	double threshold = -1;
	const MaxARTPopulation &population = static_cast<const MaxARTPopulation &>(pop);
	const Facility *pFac = getCurrentFacilityAndThreshold(population, threshold);
	string stageName = "(undefined)";

	if (population.getStudyStage() == MaxARTPopulation::PreStudy)
		stageName = "Pre-study";
	else if (population.getStudyStage() == MaxARTPopulation::PostStudy)
		stageName = "Post-study";
	else if (population.getStudyStage() == MaxARTPopulation::InStudy)
		stageName = pFac->getStageName();

	LogEvent.print(",CD4,%g,Facility,%s,Stage,%s,CD4Threshold,%g", 
			        pPerson->getCD4Count(tNow), pFac->getName().c_str(), stageName.c_str(), threshold);
}

bool EventMonitoring::isEligibleForTreatment(double t, const MaxARTPopulation &population)
{
	Person *pPerson = getPerson(0);

	// if the person has already received treatment, (s)he's still eligible
	if (pPerson->getNumberTreatmentStarted() > 0) 
		return true;

	double threshold = -1;
	const Facility *pFac = getCurrentFacilityAndThreshold(population, threshold);
	assert(pFac);
	assert(threshold >= 0);

	double cd4count = pPerson->getCD4Count(t);
	//cout << "T:" << t <<  " " << pPerson->getName() << " has CD4 " << cd4count << ", is at " << pFac->getName() << " in " 
	//	 << pFac->getStageName() << " with threshold " << threshold << endl;

	// Check the threshold
	if (cd4count < threshold)
		return true;

	return false;
}

const Facility *EventMonitoring::getCurrentFacility() const
{
	// For now, we'll just use the closest facility
	Person *pPerson = getPerson(0);
	Point2D personLocation = pPerson->getLocation();

	Facilities *pFacilities = Facilities::getInstance();
	assert(pFacilities);

	int num = pFacilities->getNumberOfFacilities();
	assert(num > 0);

	const Facility *pClosestFac = pFacilities->getFacility(0);
	double bestDist = pClosestFac->getPosition().getSquaredDistanceTo(personLocation);

	for (int i = 1 ; i < num ; i++)
	{
		const Facility *pFac = pFacilities->getFacility(i);
		double d = pFac->getPosition().getSquaredDistanceTo(personLocation);

		if (d < bestDist)
		{
			bestDist = d;
			pClosestFac = pFac;
		}
	}

	//cout << "Closest facility for " << pPerson->getName() << " at " << personLocation.x << "," << personLocation.y
	//	 << " is " << pClosestFac->getName() << " at " << pClosestFac->getPosition().x << "," << pClosestFac->getPosition().y << endl;

	return pClosestFac;
}

const Facility *EventMonitoring::getCurrentFacilityAndThreshold(const MaxARTPopulation &population, double &threshold) const
{
	const Facility *pFac = getCurrentFacility();

	threshold = -1;
	switch(population.getStudyStage())
	{
	case MaxARTPopulation::PreStudy:
		threshold = s_cd4ThresholdPreStudy;
		break;
	case MaxARTPopulation::InStudy:
		if (pFac->getStage() == Facility::ControlStage)
			threshold = s_cd4ThresholdInStudyControlStage;
		else if (pFac->getStage() == Facility::TransitionStage)
			threshold = s_cd4ThresholdInStudyTransitionStage;
		else if (pFac->getStage() == Facility::InterventionStage)
			threshold = s_cd4ThresholdInStudyInterventionStage;
		else
			abortWithMessage("Internal error: unknown MaxART facility stage");
		break;
	case MaxARTPopulation::PostStudy:
		threshold = s_cd4ThresholdPostStudy;
		break;
	default:
		abortWithMessage("Internal error: unknown MaxART study stage");
	}
	assert(threshold >= 0);

	return pFac;
}

bool EventMonitoring::isWillingToStartTreatment(double t, GslRandomNumberGenerator *pRndGen)
{
	Person *pPerson = getPerson(0);

	// Coin toss
	double x = pRndGen->pickRandomDouble();
	if (x < pPerson->getARTAcceptanceThreshold())
		return true;

	return false;
}

void EventMonitoring::fire(State *pState, double t)
{
	MaxARTPopulation &population = MAXARTPOPULATION(pState);
	GslRandomNumberGenerator *pRndGen = population.getRandomNumberGenerator();
	Person *pPerson = getPerson(0);

	assert(pPerson->isInfected());
	assert(!pPerson->hasLoweredViralLoad());
	assert(s_treatmentVLLogFrac >= 0 && s_treatmentVLLogFrac <= 1.0);

	if (isEligibleForTreatment(t, population) && isWillingToStartTreatment(t, pRndGen))
	{
		SimpactEvent::writeEventLogStart(true, "(treatment)", t, pPerson, 0);

		// Person is starting treatment, no further HIV test events will follow
		pPerson->lowerViralLoad(s_treatmentVLLogFrac, t);

		// Dropout event becomes possible
		EventDropout *pEvtDropout = new EventDropout(pPerson, t);
		population.onNewEvent(pEvtDropout);
	}
	else
	{
		// Schedule a new monitoring event
		EventMonitoring *pNewMonitor = new EventMonitoring(pPerson);
		population.onNewEvent(pNewMonitor);
	}
}

double EventMonitoring::getNewInternalTimeDifference(GslRandomNumberGenerator *pRndGen, const State *pState)
{
	// This is for the monitoring event that should be scheduled right after the
	// diagnosis event
	if (m_scheduleImmediately)
	{
		double hour = 1.0/(365.0*24.0); // an hour in a unit of a year
		return hour * pRndGen->pickRandomDouble();
	}

	assert(s_pRecheckInterval);

	const MaxARTPopulation &population = MAXARTPOPULATION(pState);
	Person *pPerson = getPerson(0);
	double currentTime = population.getTime();
	double cd4 = pPerson->getCD4Count(currentTime);
	double dt = s_pRecheckInterval->evaluate(cd4);

	assert(dt >= 0);
	return dt;
}

double EventMonitoring::s_treatmentVLLogFrac = -1;
double EventMonitoring::s_cd4ThresholdPreStudy = -1;
double EventMonitoring::s_cd4ThresholdInStudyControlStage = -1;
double EventMonitoring::s_cd4ThresholdInStudyTransitionStage = -1;
double EventMonitoring::s_cd4ThresholdInStudyInterventionStage = -1;
double EventMonitoring::s_cd4ThresholdPostStudy = -1;
PieceWiseLinearFunction *EventMonitoring::s_pRecheckInterval = 0;

void EventMonitoring::processConfig(ConfigSettings &config, GslRandomNumberGenerator *pRndGen)
{
	if (!config.getKeyValue("monitoring.cd4.threshold.prestudy", s_cd4ThresholdPreStudy, 0) ||
		!config.getKeyValue("monitoring.cd4.threshold.poststudy", s_cd4ThresholdPostStudy, 0) ||
		!config.getKeyValue("monitoring.cd4.threshold.instudy.controlstage", s_cd4ThresholdInStudyControlStage, 0) ||
		!config.getKeyValue("monitoring.cd4.threshold.instudy.transitionstage", s_cd4ThresholdInStudyTransitionStage, 0) ||
		!config.getKeyValue("monitoring.cd4.threshold.instudy.interventionstage", s_cd4ThresholdInStudyInterventionStage, 0) ||
	    !config.getKeyValue("monitoring.fraction.log_viralload", s_treatmentVLLogFrac, 0, 1))
		abortWithMessage(config.getErrorString());

	vector<double> intervalX, intervalY;
	double leftValue, rightValue;

	if (!config.getKeyValue("monitoring.interval.piecewise.cd4s", intervalX) ||
	    !config.getKeyValue("monitoring.interval.piecewise.times", intervalY) ||
	    !config.getKeyValue("monitoring.interval.piecewise.left", leftValue) ||
	    !config.getKeyValue("monitoring.interval.piecewise.right", rightValue))
		abortWithMessage(config.getErrorString());

	for (size_t i = 0 ; i < intervalX.size()-1 ; i++)
	{
		if (intervalX[i+1] < intervalX[i])
			abortWithMessage("CD4 values must be increasing in 'monitoring.interval.piecewise.cd4s'");
	}

	if (intervalX.size() < 1)
		abortWithMessage("CD4 value list 'monitoring.interval.piecewise.cd4s' must contain at least one element");

	if (intervalX.size() != intervalY.size())
		abortWithMessage("Lists 'monitoring.interval.piecewise.cd4s' and 'monitoring.interval.piecewise.times' must contain the same number of elements");

	vector<Point2D> points;

	for (size_t i = 0 ; i < intervalX.size() ; i++)
		points.push_back(Point2D(intervalX[i], intervalY[i]));

	delete s_pRecheckInterval;
	s_pRecheckInterval = new PieceWiseLinearFunction(points, leftValue, rightValue);
}

void EventMonitoring::obtainConfig(ConfigWriter &config)
{
	assert(s_pRecheckInterval);

	const vector<Point2D> &points = s_pRecheckInterval->getPoints();
	vector<double> intervalX, intervalY;

	for (size_t i = 0 ; i < points.size() ; i++)
	{
		intervalX.push_back(points[i].x);
		intervalY.push_back(points[i].y);
	}

	if (!config.addKey("monitoring.cd4.threshold.prestudy", s_cd4ThresholdPreStudy) ||
		!config.addKey("monitoring.cd4.threshold.poststudy", s_cd4ThresholdPostStudy) ||
		!config.addKey("monitoring.cd4.threshold.instudy.controlstage", s_cd4ThresholdInStudyControlStage) ||
		!config.addKey("monitoring.cd4.threshold.instudy.transitionstage", s_cd4ThresholdInStudyTransitionStage) ||
		!config.addKey("monitoring.cd4.threshold.instudy.interventionstage", s_cd4ThresholdInStudyInterventionStage) ||
	    !config.addKey("monitoring.fraction.log_viralload", s_treatmentVLLogFrac) ||
	    !config.addKey("monitoring.interval.piecewise.cd4s", intervalX) ||
	    !config.addKey("monitoring.interval.piecewise.times", intervalY) ||
	    !config.addKey("monitoring.interval.piecewise.left", s_pRecheckInterval->getLeftValue()) ||
	    !config.addKey("monitoring.interval.piecewise.right", s_pRecheckInterval->getRightValue()) )
		abortWithMessage(config.getErrorString());
}

ConfigFunctions monitoringConfigFunctions(EventMonitoring::processConfig, EventMonitoring::obtainConfig, "EventMonitoring");

JSONConfig monitoringJSONConfig(R"JSON(
        "EventMonitoring" : {
            "depends": null,
            "params": [
				[ "monitoring.cd4.threshold.prestudy", 350 ],
				[ "monitoring.cd4.threshold.poststudy", 350 ],
				[ "monitoring.cd4.threshold.instudy.controlstage", 350 ],
				[ "monitoring.cd4.threshold.instudy.transitionstage", "inf" ],
				[ "monitoring.cd4.threshold.instudy.interventionstage", "inf" ],
                [ "monitoring.fraction.log_viralload", 0.7 ]
            ],
            "info": [
                "When a person is diagnosed (or 're-diagnosed' after a dropout), monitoring",
                "events will be scheduled using an interval that depends on the CD4 count.",
                "When such an event fires, and the person's CD4 count is below the specified",
                "CD4 threshold, the person may start ART treatment, if he/she is willing",
                "to do so (see person settings). ",
                "",
                "If the person is treated, the SPVL will be lowered in such a way that on a ",
                "logarithmic scale the new value equals the specified fraction of the original",
                "viral load."
            ]
        },

        "EventMonitoring_interval" : {
            "depends": null,
            "params": [
                [ "monitoring.interval.piecewise.cd4s", "200,350" ],
                [ "monitoring.interval.piecewise.times", "0.25,0.25" ],
                [ "monitoring.interval.piecewise.left", 0.16666 ],
                [ "monitoring.interval.piecewise.right", 0.5 ]
            ],
            "info": [
                "These parameters specify the interval with which monitoring events will take",
                "place. This is determined by a piecewise linear function, which is a function",
                "of the person's CD4 count and which will return the interval (the unit is one",
                "year).",
                "",
                "The 'monitoring.interval.piecewise.cd4s' specify the x-values of this ",
                "piecewise linear function (comma separated list), while ",
                "'monitoring.interval.piecewise.times' specified the corresponding y-values. ",
                "For an x-value (CD4 count) that's smaller than the smallest value in the list,",
                "the value in 'monitoring.interval.piecewise.left' will be returned. For an",
                "x-value that's larger than the largest value in the list, the value in",
                "'monitoring.interval.piecewise.right' will be returned."
            ]
        })JSON");

