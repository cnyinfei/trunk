//##########################################################################
//#                                                                        #
//#                            CLOUDCOMPARE                                #
//#                                                                        #
//#  This program is free software; you can redistribute it and/or modify  #
//#  it under the terms of the GNU General Public License as published by  #
//#  the Free Software Foundation; version 2 of the License.               #
//#                                                                        #
//#  This program is distributed in the hope that it will be useful,       #
//#  but WITHOUT ANY WARRANTY; without even the implied warranty of        #
//#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         #
//#  GNU General Public License for more details.                          #
//#                                                                        #
//#          COPYRIGHT: EDF R&D / TELECOM ParisTech (ENST-TSI)             #
//#                                                                        #
//##########################################################################

#include "ccMinimumSpanningTreeForNormsDirection.h"

//CCLib
#include <ReferenceCloud.h>

//qCC_db
#include <ccLog.h>
#include <ccPointCloud.h>
#include <ccScalarField.h>

//Qt
#include <QInputDialog>

//system
#include <set>
#include <map>
#include <vector>
#include <queue>

//! Weighted graph edge
class Edge
{
public:

	//! Unique edge key type
	typedef std::pair<size_t, size_t> Key;

	//! Returns the unique key of an edge (no vertex order)
	inline static Key ConstructKey(size_t v1, size_t v2)
	{
		return v1 > v2 ? std::make_pair(v2, v1) : std::make_pair(v1, v2);
	}

	//! Default constructor
	Edge(size_t v1, size_t v2, double weight)
		: m_key(ConstructKey(v1, v2))
		, m_weight(weight)
	{
		assert(m_weight >= 0);
	}

	//! Strict weak ordering operator (required by std::priority_queue)
	inline bool operator < (const Edge& other) const
	{
		return m_weight > other.m_weight;
	}

	//! Returns first vertex (index)
	const size_t& v1() const { return m_key.first; }
	//! Returns second vertex (index)
	const size_t& v2() const { return m_key.second; }

protected:

	//! Unique key
	Key m_key;
	
	//! Associated weight
	double m_weight;
};

//! Generic graph structure
class Graph
{
public:

	//! Default constructor
	Graph() {}

	//! Reserves memory for graph
	/** Must be called before using the structure!
		Clears the structure as well.
	**/
	bool reserve(size_t vertexCount)
	{
		m_edges.clear();

		try
		{
			m_vertexNeighbors.resize(vertexCount, std::set<size_t>());
		}
		catch(std::bad_alloc)
		{
			//not enough memory
			return false;
		}

		return true;
	}

	//! Returns the number of vertices
	size_t vertexCount() const { return m_vertexNeighbors.size(); }

	//! Returns the number of edges
	size_t edgeCount() const { return m_edges.size(); }

	//! Returns the weight associated to an edge (if it exists - returns -1 otherwise)
	double weight(size_t v1, size_t v2) const
	{
		assert(v1 < m_vertexNeighbors.size() && v2 < m_vertexNeighbors.size());

		// try to find the corresponding edge (otherwise return -1)
		std::map<Edge::Key, double>::const_iterator it = m_edges.find(Edge::ConstructKey(v1, v2));

		return it == m_edges.end() ? -1 : it->second;
	}

	//! Adds or updates the edge (v1,v2)
	void setEdge(size_t v1, size_t v2, double weight = 0)
	{
		assert(v1 < m_vertexNeighbors.size() && v2 < m_vertexNeighbors.size());

		m_edges.insert(std::make_pair(Edge::ConstructKey(v1, v2), weight));
		m_vertexNeighbors[v1].insert(v2);
		m_vertexNeighbors[v2].insert(v1);
	}

	//! Returns the set of edges connected to a given vertex
	inline const std::set<size_t>& getVertexNeighbors(size_t index) const
	{
		return m_vertexNeighbors[index];
	}

protected:

	//! Graph edges
	std::map<Edge::Key, double> m_edges;

	//! Set of neighbors for each vertex
	std::vector<std::set<size_t> > m_vertexNeighbors;
};

static bool ResolveNormalsWithMST(ccPointCloud* cloud, const Graph& graph, CCLib::GenericProgressCallback* progressCb = 0)
{
	assert(cloud && cloud->hasNormals());

//#define COLOR_PATCHES
#ifdef COLOR_PATCHES
	//Test: color patches
	cloud->setRGBColor(ccColor::white);
	cloud->showColors(true);

	//Test: arrival time
	int sfIdx = cloud->getScalarFieldIndexByName("MST arrival time");
	if (sfIdx < 0)
		sfIdx = cloud->addScalarField("MST arrival time");
	ccScalarField* sf = static_cast<ccScalarField*>(cloud->getScalarField(sfIdx));
	sf->fill(NAN_VALUE);
	cloud->setCurrentDisplayedScalarField(sfIdx);
#endif

	//reset
	std::priority_queue<Edge> priorityQueue;
	std::vector<bool> visited;
	unsigned visitedCount = 0;

	size_t vertexCount = graph.vertexCount();

	//instantiate the 'visited' table
	try
	{
		visited.resize(vertexCount,false);
	}
	catch(std::bad_alloc)
	{
		//not enough memory
		return false;
	}

	//progress notification
	CCLib::NormalizedProgress* nProgress(0);
	if (progressCb)
	{
		progressCb->reset();
		progressCb->setMethodTitle("Orient normals (MST)");
		progressCb->setInfo(qPrintable(QString("Compute Minimum spanning tree\nPoints: %1\nEdges: %2").arg(vertexCount).arg(graph.edgeCount())));
		nProgress = new CCLib::NormalizedProgress(progressCb,static_cast<unsigned>(vertexCount));
		progressCb->start();
	}

	//while unvisited vertices remain...
	size_t firstUnvisitedIndex = 0;
	size_t patchCount = 0;
	size_t inversionCount = 0;
	while (visitedCount < vertexCount)
	{
		//find the first not-yet-visited vertex
		while (visited[firstUnvisitedIndex])
			++firstUnvisitedIndex;

		//set it as "visited"
		{
			visited[firstUnvisitedIndex] = true;
			++visitedCount;
			//add its neighbors to the priority queue
			const std::set<size_t>& neighbors = graph.getVertexNeighbors(firstUnvisitedIndex);
			for (std::set<size_t>::const_iterator it = neighbors.begin(); it != neighbors.end(); ++it)
				priorityQueue.push(Edge(firstUnvisitedIndex, *it, graph.weight(firstUnvisitedIndex, *it)));

			if (nProgress && !nProgress->oneStep())
				break;
		}

#ifdef COLOR_PATCHES
		ccColor::Rgb patchCol = ccColor::Generator::Random();
		cloud->setPointColor(static_cast<unsigned>(firstUnvisitedIndex), patchCol.rgb);
		sf->setValue(static_cast<unsigned>(firstUnvisitedIndex),static_cast<ScalarType>(visitedCount));
#endif

		while(!priorityQueue.empty() && visitedCount < vertexCount)
		{
			//process next edge (with the lowest 'weight')
			Edge element = priorityQueue.top();
			priorityQueue.pop();

			//there should only be (at most) one unvisited vertex in the edge
			size_t v = 0;
			if (!visited[element.v1()])
				v = element.v1();
			else if (!visited[element.v2()])
				v = element.v2();
			else
				continue;

			//invert normal if necessary (DO THIS BEFORE SETTING THE VERTEX AS 'VISITED'!)
			const CCVector3& N1 = cloud->getPointNormal(static_cast<unsigned>(element.v1()));
			const CCVector3& N2 = cloud->getPointNormal(static_cast<unsigned>(element.v2()));
			if (N1.dot(N2) < 0)
			{
				if (!visited[element.v1()])
				{
					cloud->setPointNormal(static_cast<unsigned>(v), -N1);
				}
				else
				{
					cloud->setPointNormal(static_cast<unsigned>(v), -N2);
				}
				++inversionCount;
			}

			//set it as "visited"
			{
				visited[v] = true;
				++visitedCount;
				//add its neighbors to the priority queue
				const std::set<size_t>& neighbors = graph.getVertexNeighbors(v);
				for (std::set<size_t>::const_iterator it = neighbors.begin(); it != neighbors.end(); ++it)
					priorityQueue.push(Edge(v, *it, graph.weight(v,*it)));
			}

#ifdef COLOR_PATCHES
			cloud->setPointColor(static_cast<unsigned>(v), patchCol);
			sf->setValue(static_cast<unsigned>(v),static_cast<ScalarType>(visitedCount));
#endif
			if (nProgress && !nProgress->oneStep())
			{
				visitedCount = static_cast<unsigned>(vertexCount); //early stop
				break;
			}
		}

		//new patch
		++patchCount;
	}

#ifdef COLOR_PATCHES
	sf->computeMinAndMax();
	cloud->showSF(true);
#endif

	if (nProgress)
	{
		delete nProgress;
		nProgress = 0;
		progressCb->stop();
	}

	ccLog::Print(QString("[ResolveNormalsWithMST] Patches = %1 / Inversions: %2").arg(patchCount).arg(inversionCount));

	return true;
}

#define MST_USE_KNN

static bool ComputeMSTGraphAtLevel(	const CCLib::DgmOctree::octreeCell& cell,
									void** additionalParameters,
									CCLib::NormalizedProgress* nProgress/*=0*/)
{
	//parameters
	Graph* graph = static_cast<Graph*>(additionalParameters[0]);
	ccPointCloud* cloud = static_cast<ccPointCloud*>(additionalParameters[1]);

	//structure for the nearest neighbor search
#ifdef MST_USE_KNN

	unsigned kNN = *static_cast<unsigned*>(additionalParameters[2]);

	CCLib::DgmOctree::NearestNeighboursSearchStruct nNSS;
	nNSS.level								= cell.level;
	nNSS.minNumberOfNeighbors				= kNN+1; //+1 because we'll get the query point itself!
	cell.parentOctree->getCellPos(cell.truncatedCode,cell.level,nNSS.cellPos,true);
	cell.parentOctree->computeCellCenter(nNSS.cellPos,cell.level,nNSS.cellCenter);
#else

	PointCoordinateType radius = *static_cast<PointCoordinateType*>(additionalParameters[2]);

	CCLib::DgmOctree::NearestNeighboursSphericalSearchStruct nNSS;
	nNSS.level								= cell.level;
	nNSS.prepare(radius,cell.parentOctree->getCellSize(nNSS.level));
	cell.parentOctree->getCellPos(cell.truncatedCode,cell.level,nNSS.cellPos,true);
	cell.parentOctree->computeCellCenter(nNSS.cellPos,cell.level,nNSS.cellCenter);
#endif

	unsigned n = cell.points->size(); //number of points in the current cell

	//we already know some of the neighbours: the points in the current cell!
	{
		try
		{
			nNSS.pointsInNeighbourhood.resize(n);
		}
		catch (.../*const std::bad_alloc&*/) //out of memory
		{
			return false;
		}

		CCLib::DgmOctree::NeighboursSet::iterator it = nNSS.pointsInNeighbourhood.begin();
		for (unsigned i=0; i<n; ++i,++it)
		{
			it->point = cell.points->getPointPersistentPtr(i);
			it->pointIndex = cell.points->getPointGlobalIndex(i);
		}
	}
	nNSS.alreadyVisitedNeighbourhoodSize = 1;

	//for each point in the cell
	for (unsigned i=0; i<n; ++i)
	{
		cell.points->getPoint(i,nNSS.queryPoint);

		//look for neighbors in a sphere
#ifdef MST_USE_KNN
		unsigned neighborCount = cell.parentOctree->findNearestNeighborsStartingFromCell(nNSS,false);
		neighborCount = std::min(neighborCount,kNN+1);
#else
		//warning: there may be more points at the end of nNSS.pointsInNeighbourhood than the actual nearest neighbors (neighborCount)!
		unsigned neighborCount = cell.parentOctree->findNeighborsInASphereStartingFromCell(nNSS,radius,false);
		if (neighborCount < 2)
		{
			//second chance with nearest neighbor!
			CCLib::DgmOctree::NearestNeighboursSearchStruct nNSS2;
			nNSS2.level								= cell.level;
			nNSS2.minNumberOfNeighbors				= 2; //+1 because we'll get the query point itself!
			nNSS2.cellPos[0] = nNSS.cellPos[0];
			nNSS2.cellPos[1] = nNSS.cellPos[1];
			nNSS2.cellPos[2] = nNSS.cellPos[2];
			nNSS2.cellCenter[0] = nNSS.cellCenter[0];
			nNSS2.cellCenter[1] = nNSS.cellCenter[1];
			nNSS2.cellCenter[2] = nNSS.cellCenter[2];
			cell.parentOctree->computeCellCenter(nNSS2.cellPos,cell.level,nNSS2.cellCenter);

			try
			{
				nNSS2.pointsInNeighbourhood.resize(n);
			}
			catch (.../*const std::bad_alloc&*/) //out of memory
			{
				return false;
			}
			CCLib::DgmOctree::NeighboursSet::iterator it = nNSS2.pointsInNeighbourhood.begin();
			for (unsigned i=0; i<n; ++i,++it)
			{
				it->point = cell.points->getPointPersistentPtr(i);
				it->pointIndex = cell.points->getPointGlobalIndex(i);
			}
			nNSS2.alreadyVisitedNeighbourhoodSize = 1;

			neighborCount = cell.parentOctree->findNearestNeighborsStartingFromCell(nNSS2,false);
			neighborCount = std::min<unsigned>(neighborCount,2);

			nNSS.pointsInNeighbourhood = nNSS2.pointsInNeighbourhood;
		}
#endif

		//current point index
		unsigned index = cell.points->getPointGlobalIndex(i);
		const CCVector3& N1 = cloud->getPointNormal(static_cast<unsigned>(index));
		//const CCVector3* P1 = cloud->getPoint(static_cast<unsigned>(index));
		for (unsigned j=0; j<neighborCount; ++j)
		{
			//current neighbor index
			const unsigned& neighborIndex = nNSS.pointsInNeighbourhood[j].pointIndex;
			if (index != neighborIndex)
			{
				const CCVector3& N2 = cloud->getPointNormal(static_cast<unsigned>(neighborIndex));
				double weight = 0;
				//dot product
				weight = std::max(0.0,1.0 - fabs(N1.dot(N2)));
				
				//distance
				//weight = sqrt(nNSS.pointsInNeighbourhood[j].squareDistd);

				//mutual dot product
				//const CCVector3* P2 = cloud->getPoint(static_cast<unsigned>(neighborIndex));
				//CCVector3 uAB = *P2 - *P1;
				//uAB.normalize();
				//weight = (fabs(CCVector3::vdot(uAB.u,N1) + fabs(CCVector3::vdot(uAB.u,N2)))) / 2.0;

				graph->setEdge(index,neighborIndex,weight);
			}
		}

		if (nProgress && !nProgress->oneStep())
			return false;
	}

	return true;
}

bool ccMinimumSpanningTreeForNormsDirection::Process(	ccPointCloud* cloud,
														CCLib::GenericProgressCallback* progressCb/*=0*/,
														CCLib::DgmOctree* _octree/*=0*/)
{
	assert(cloud);
	if (!cloud->hasNormals())
	{
		ccLog::Warning(QString("Cloud '%1' has no normals!").arg(cloud->getName()));
		return false;
	}

	//ask for parameter
	bool ok;
#ifdef MST_USE_KNN
	unsigned kNN = static_cast<unsigned>(QInputDialog::getInt(0,"Neighborhood size", "Neighbors", 6, 1, 1000, 1, &ok));
	if (!ok)
		return false;
#else
	PointCoordinateType radius = static_cast<PointCoordinateType>(QInputDialog::getDouble(0,"Neighborhood radius", "radius", cloud->getOwnBB().getDiagNorm() * 0.01, 0, 1.0e9, 6, &ok));
	if (!ok)
		return false;
#endif

	//build octree if necessary
	CCLib::DgmOctree* octree = _octree;
	if (!octree)
	{
		octree = new CCLib::DgmOctree(cloud);
		if (octree->build(progressCb) <= 0)
		{
			ccLog::Warning(QString("Failed to compute octree on cloud '%1'").arg(cloud->getName()));
			delete octree;
			return false;
		}
	}
		
#ifdef MST_USE_KNN
	uchar level = octree->findBestLevelForAGivenPopulationPerCell(kNN*2);
#else
	uchar level = octree->findBestLevelForAGivenNeighbourhoodSizeExtraction(radius);
#endif

	bool result = true;
	try
	{
		Graph graph;
		if (!graph.reserve(cloud->size()))
		{
			//not enough memory!
			result = false;
		}

		//parameters
		void* additionalParameters[3] = {	reinterpret_cast<void*>(&graph),
											reinterpret_cast<void*>(cloud),
#ifdef MST_USE_KNN
											reinterpret_cast<void*>(&kNN)
#else
											reinterpret_cast<void*>(&radius)
#endif
										};

		if (octree->executeFunctionForAllCellsAtLevel(	level,
														&ComputeMSTGraphAtLevel,
														additionalParameters,
														false, //not compatible with parallel strategies!
														progressCb,
														"Build Spanning Tree") == 0)
		{
			//something went wrong
			ccLog::Warning(QString("Failed to compute Spanning Tree on cloud '%1'").arg(cloud->getName()));
			result = false;
		}
		else
		{
			if (!ResolveNormalsWithMST(cloud,graph,progressCb))
			{
				//something went wrong
				ccLog::Warning(QString("Failed to compute Minimum Spanning Tree on cloud '%1'").arg(cloud->getName()));
				result = false;
			}
		}
	}
	catch(...)
	{
		ccLog::Error(QString("Process failed on cloud '%1'").arg(cloud->getName()));
		result = false;
	}

	if (octree && !_octree)
	{
		delete octree;
		octree = 0;
	}

	return result;
}
