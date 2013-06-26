/*
 * SpaceTimeDataController.h
 *
 *  Created on: Jul 9, 2012
 *      Author: pat2
 */

#ifndef SpaceTimeDataController_H_
#define SpaceTimeDataController_H_
#include "FileData.hpp"
#include "ImageTraceAttributes.hpp"
#include "ProcessTimeline.hpp"
#include "FilteredBaseData.hpp"
#include "FilterSet.hpp"

namespace TraceviewerServer
{

	class SpaceTimeDataController
	{
	public:

		SpaceTimeDataController(FileData*);
		virtual ~SpaceTimeDataController();
		void setInfo(Long, Long, int);
		ProcessTimeline* getNextTrace();
		void addNextTrace(ProcessTimeline*);
		void fillTraces();
		ProcessTimeline* fillTrace(bool);
		void applyFilters(FilterSet filters);
		//The number of processes in the database, independent of the current display size
		int getNumRanks();

		 int* getValuesXProcessID();
		 short* getValuesXThreadID();

		std::string getExperimentXML();
		ImageTraceAttributes* attributes;
		ProcessTimeline** traces;
		int tracesLength;
	private:
		void resetTraces();

		ImageTraceAttributes* oldAttributes;

		FilteredBaseData* dataTrace;
		int headerSize;

		// The minimum beginning and maximum ending time stamp across all traces (in microseconds).
		Long maxEndTime, minBegTime;

		int height;
		string experimentXML;
		string fileTrace;

		bool tracesInitialized;

		static const int DEFAULT_HEADER_SIZE = 24;

	};

} /* namespace TraceviewerServer */
#endif /* SpaceTimeDataController_H_ */