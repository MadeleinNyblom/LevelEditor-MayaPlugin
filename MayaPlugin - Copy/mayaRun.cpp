//Madelein Nyblom
//2018-10-16
//UD1447

#include "maya_includes.h"
#include <maya/MTimer.h>
#include <iostream>
#include <algorithm>
#include <vector>
#include <queue>
#include "comLib.h"
#include <string.h>

#pragma comment(lib, "UD1447Test.lib")

using namespace std;
MCallbackIdArray callbackIdArray;
MObject m_node;
MStatus status = MS::kSuccess;

MCallbackId topId;

float gTotalTime = 0.0f;
float gLastTime = 0.0f;

bool initBool = false;

enum NODE_TYPE { TRANSFORM, MESH };
MTimer gTimer;

// keep track of created meshes to maintain them
//queue<MObject> newMeshes;
std::vector<MObject> newMeshes;

ComLib comLib("sharedMem", 50, PRODUCER);

//Structs for message
//----------------------------------------------
//Mesh
struct rayLibMeshHeader {
	int vertexCount;
};

//Matrix
struct Matrix {
	float m0, m4, m8, m12;
	float m1, m5, m9, m13;
	float m2, m6, m10, m14;
	float m3, m7, m11, m15;
};

struct Vector3 {
	float x;
	float y;
	float z;
};

//Header for message
struct messageHeader
{
	char command[64];
	int nrOfMessages;
};

//Header for model
struct modelHeader
{
	char mModelName[64];
};
//------------------------------------------------------------

//Struct or variables for messages that happens on multiple meshes at the same time during one frame,
//or objects/vertices position's cahnging but avoiding spamming that information to the rayLib
//------------------------------------------------------------
//Struct for keeping world matrix messages in queue
struct queueWorldMatrixMessage
{
	MString nodeName;
	Matrix matrixToBeQueued;
};

//Struct for keeping vertex messages in queue
struct queueTopologyChangedMessage
{
	MString nodeName;
	int vertexCount;
	float* vertices;
	float* normals;
	float* uvs;
};
//-------------------------------------------------------------

//Vectors that will handle the different queues that will be sent in timerCallback
//--------------------------------------------------------------------
std::vector<queueWorldMatrixMessage> queuedWorldMatrixMessages;
std::vector< queueTopologyChangedMessage> queuedTopologyChangedMessages;
std::vector<MString> queuedNodesRemoved;

//--------------------------------------------------------------------

//Camera stuff
//--------------------------------------------------------------------
Vector3 gCamUpDir, gCamTarget, gCamPos;
double gFovY;
int gIsOrtho = 0;

bool camBool = false;
//--------------------------------------------------------------------

/*
 * how Maya calls this method when a node is added.
 * new POLY mesh: kPolyXXX, kTransform, kMesh
 * new MATERIAL : kBlinn, kShadingEngine, kMaterialInfo
 * new LIGHT    : kTransform, [kPointLight, kDirLight, kAmbientLight]
 * new JOINT    : kJoint
 */

//Camera changed
//---------------------------------------------------------------------
void cameraWorldMatrixChanged(MObject &transformNode, MDagMessage::MatrixModifiedFlags &modified, void *clientData)
{
	MDagPath path;
	MFnDagNode(transformNode).getPath(path);

	MFnCamera cam(path);
	MFnTransform camTransform(path);

	double camUpDir[3], camTarget[4], camPos[4];

	cam.upDirection(MSpace::kWorld).get(camUpDir);
	cam.centerOfInterestPoint(MSpace::kWorld).get(camTarget);
	cam.eyePoint(MSpace::kWorld).get(camPos);
	
	gFovY = cam.verticalFieldOfView();

	gCamUpDir = { (float)camUpDir[0], (float)camUpDir[1], (float)camUpDir[2] };
	gCamTarget = { (float)camTarget[0], (float)camTarget[1], (float)camTarget[2] };
	gCamPos = { (float)camPos[0], (float)camPos[1], (float)camPos[2] };

	gIsOrtho = 0;

	MString camName = cam.name();

	if (camName != "perspShape")
	{
		gIsOrtho = 1;

		//For getting zoom in ortographic view, doesn't have support for that in rayLib
		/*float orthoWidth = cam.orthoWidth();

		MStreamUtils::stdOutStream() << orthoWidth << "\n";

		if (camName == "sideShape")
		{
			gCamPos.x = orthoWidth;
		}

		else if (camName == "topShape")
		{
			gCamPos.y = orthoWidth;
		}

		else
		{
			gCamPos.z = orthoWidth;
		}*/

	}

	camBool = true;
}

void camChanged(const MString &str, MObject &node, void *clientData)
{
	MDagPath path;
	MFnDagNode(node).getPath(path);
	MFnCamera cam(path);
	MFnTransform camTransform(path);

	MStreamUtils::stdOutStream() << "Camera changed" << "\n";

	//MCallbackId tempID;

	//tempID = MDagMessage::addWorldMatrixModifiedCallback(path, cameraWorldMatrixChanged, NULL, &status);
	//callbackIdArray.append(tempID);
}

 //World matrix changed
void nodeWorldMatrixChanged(MObject &transformNode, MDagMessage::MatrixModifiedFlags &modified, void *clientData)
{
	//Find path so matrices and mesh node can be found
	MDagPath path;
	MFnDagNode(transformNode).getPath(path);

	MMatrix worldMatrix = MMatrix(path.inclusiveMatrix());		//Inclusive Matrix gives us world Matrix
	MMatrix localMatrix = MMatrix(path.exclusiveMatrix());		//Exclusive Matrix gives us local matrix

	MFnMesh mMesh(path);

	MString mMeshName = mMesh.name();

	Matrix worldMatrixChanged = { worldMatrix[0][0], worldMatrix[1][0], worldMatrix[2][0], worldMatrix[3][0],
	worldMatrix[0][1], worldMatrix[1][1], worldMatrix[2][1], worldMatrix[3][1],
	worldMatrix[0][2], worldMatrix[1][2], worldMatrix[2][2], worldMatrix[3][2],
	worldMatrix[0][3], worldMatrix[1][3], worldMatrix[2][3], worldMatrix[3][3] };


	//Check if a message for this node already is in the queue, if so replace with new value
	bool exist = false;
	for (int i = 0; i < queuedWorldMatrixMessages.size(); i++)
	{
		if (queuedWorldMatrixMessages[i].nodeName == mMeshName)
		{
			exist = true;
			queuedWorldMatrixMessages[i].matrixToBeQueued = worldMatrixChanged;
		}
	}

	//If a message for this node hasn't been made, push the node and the message
	if (exist == false)
	{
		queueWorldMatrixMessage tMessage;
		tMessage.nodeName = mMeshName;
		tMessage.matrixToBeQueued = worldMatrixChanged;
		queuedWorldMatrixMessages.push_back(tMessage);
	}
}

//Callback function that handles when vertices connection is made to the dependency graph (triangulated, topology changes, new mesh added)
//----------------------------------------------------------
void vtxPlugConnected(MPlug & srcPlug, MPlug & destPlug, bool made, void* clientData)
{
	if (srcPlug.partialName() == "out" && destPlug.partialName() == "i")		//Check if it is a output mesh that has been connected to the DG
	{
		if (made == 1)				//Check if connection has been made to the DG, 0 means that vertices' connection was broken from the DG
		{
			MStreamUtils::stdOutStream() << "Vertices connected for new node" << "\n";

			//Get the mesh
			MDagPath mPath;	
			MFnDagNode(destPlug.node()).getPath(mPath);
			MFnMesh mMesh(mPath);										//The mesh is accessed through the plug's path

			rayLibMeshHeader rMesh;										//Mesh struct, used more later when more variables are added to them

			
			//MStreamUtils::stdOutStream() << triangleVertices << "\n";

			//Vertices
			MPointArray vertexArray;

			mMesh.getPoints(vertexArray, MSpace::kObject);
			MIntArray vertexCount;
			MIntArray vertexList;

			mMesh.getVertices(vertexCount, vertexList);

			//Normals
			MFloatVectorArray normalArray;
			mMesh.getNormals(normalArray, MSpace::kPreTransform);

			MIntArray normalCount;
			MIntArray normalList;
			mMesh.getNormalIds(normalCount, normalList);

			//Uvs
			MFloatArray uArray;
			MFloatArray vArray;

			mMesh.getUVs(uArray, vArray);

			MIntArray uvCounts;
			MIntArray uvList;
			mMesh.getAssignedUVs(uvCounts, uvList, NULL);

			//Print length of indices
			MStreamUtils::stdOutStream() << "Vertex list length: " << vertexList.length() << "\n";
			MStreamUtils::stdOutStream() << "Normal list length: " << normalList.length() << "\n";
			MStreamUtils::stdOutStream() << "Uv list length: " << uvList << "\n";

			//Add vertices, normals and uvs
			float* vertices = new float[vertexList.length() * 3];
			float* normals = new float[vertexList.length() * 3];
			float* uvs = new float[vertexList.length() * 2];
			rMesh.vertexCount = vertexList.length();

			//Add vertices, normals and uvs to the arrays, using the indices to append them to the right place
			int vnCount = 0;
			int uvCount = 0;
			for (int i = 0; i < vertexList.length(); i++)
			{
				vertices[vnCount] = vertexArray[vertexList[i]].x;
				vertices[vnCount + 1] = vertexArray[vertexList[i]].y;
				vertices[vnCount + 2] = vertexArray[vertexList[i]].z;

				normals[vnCount] = normalArray[normalList[i]][0];
				normals[vnCount + 1] = normalArray[normalList[i]][1];
				normals[vnCount + 2] = normalArray[normalList[i]][2];

				uvs[uvCount] = uArray[uvList[i]];
				uvs[uvCount + 1] = 1 - vArray[uvList[i]];

				vnCount += 3;
				uvCount += 2;
			}

			
			//Check if the mesh is new is the scene by looping through the new meshes array
			MObject nodeTest(mPath.node());

			int index = -1;
			for (int i = 0; i < newMeshes.size(); i++)
			{
				if (newMeshes[i] == nodeTest)
				{
					index = i;
					break;
				}
			}
			
			if (index != -1)			//If the mesh is found it is new and need to inform that to the rayLib, else it already exists in the scene and topology has changed
			{
				MStreamUtils::stdOutStream() << "New Node added heree" << "\n";


				//Header for message (command, meshName)
				messageHeader msgHeader = { "nodeAdded", 1 };	// "nodeAdded" is used since here a new node is added
				modelHeader mHeader;
				strcpy(mHeader.mModelName, mMesh.name().asChar());	// Gets the name of the mesh in full path (pCube1 | pCubeShape1)
				
				//Memcpy over everything into the message
				size_t msgSize = (sizeof(messageHeader) + sizeof(modelHeader) + sizeof(rayLibMeshHeader) + ((sizeof(float) * (rMesh.vertexCount * 3)) * 2) + (sizeof(float) * (rMesh.vertexCount * 2)));		//Define size for everything that will be copied
				const char* message = new char[msgSize];																						//Allocate memory for the message																				

				memcpy((char*)message, &msgHeader, sizeof(messageHeader));													//Memcpy header into the message (command, nrOfMessages)
				memcpy((char*)message + sizeof(messageHeader), &mHeader, sizeof(modelHeader));								//Memcpy model header (modelName)
				memcpy((char*)message + sizeof(messageHeader) + sizeof(modelHeader), &rMesh, sizeof(rayLibMeshHeader));		//Memcpy vertex count into the message
				memcpy((char*)message + sizeof(messageHeader) + sizeof(modelHeader) + sizeof(rayLibMeshHeader), vertices,
					sizeof(float) * (rMesh.vertexCount * 3));																//Memcpy the vertices into the message
				memcpy((char*)message + sizeof(messageHeader) + sizeof(modelHeader) + sizeof(rayLibMeshHeader) + (sizeof(float) * (rMesh.vertexCount * 3)),
					normals, sizeof(float) * (rMesh.vertexCount * 3));
				memcpy((char*)message + sizeof(messageHeader) + sizeof(modelHeader) + sizeof(rayLibMeshHeader) + ((sizeof(float) * (rMesh.vertexCount * 3)) * 2),
					uvs, sizeof(float) * (rMesh.vertexCount * 2));

				//Send message to eayLib through comLib
				if (comLib.send(message, msgSize))
				{
					MStreamUtils::stdOutStream() << "Message (Node Added) sent" << "\n";
				}

				delete[] message;							//Delete message

				newMeshes.erase(newMeshes.begin() + index);		//Delete mesh from new meshes, since it isn't new anymore
				//MStreamUtils::stdOutStream() << newMeshes.size() << "\n";

				MString command("polyTriangulate " + mMesh.name());
				MStreamUtils::stdOutStream() << command << "\n";
				int result;
				MGlobal::executeCommand(command, result, false, true);
			}
			else
			{
				//Check if the mesh already have a topology message queued, if it does it should be replaces with the newer one 
				bool exists = false;
				for (int i = 0; i < queuedTopologyChangedMessages.size(); i++)
				{
					if (queuedTopologyChangedMessages[i].nodeName == mMesh.name())
					{
						exists = true;		//If it exists, the existing message will just be changed and nothing new will be  pushed into the vector
						
						if (queuedTopologyChangedMessages[i].vertexCount != rMesh.vertexCount)				//If vertex count is different, allocate new mem
						{
							delete[] queuedTopologyChangedMessages[i].vertices;								//Delete old array to be able to allocate new
							queuedTopologyChangedMessages[i].vertices = new float[rMesh.vertexCount * 3];
							delete[] queuedTopologyChangedMessages[i].normals;								//Delete old array to be able to allocate new
							queuedTopologyChangedMessages[i].normals = new float[rMesh.vertexCount * 3];
							delete[] queuedTopologyChangedMessages[i].uvs;								//Delete old array to be able to allocate new
							queuedTopologyChangedMessages[i].uvs = new float[rMesh.vertexCount * 2];
						}
						
						queuedTopologyChangedMessages[i].vertexCount = rMesh.vertexCount;												//Set vertexcount
						memcpy((char*)queuedTopologyChangedMessages[i].vertices, vertices, sizeof(float) * (rMesh.vertexCount * 3));	//Memcpy the new vertices
						memcpy((char*)queuedTopologyChangedMessages[i].normals, normals, sizeof(float) * (rMesh.vertexCount * 3));
						memcpy((char*)queuedTopologyChangedMessages[i].uvs, uvs, sizeof(float) * (rMesh.vertexCount * 2));
						break;
					}
				}

				if (!exists)		//If the message doesn't exists it needs to be pushed into the vector
				{
					MStreamUtils::stdOutStream() << "Vertices connected, changes made" << "\n";

					queueTopologyChangedMessage topologyChangedMessage = { mMesh.name(), rMesh.vertexCount };
					topologyChangedMessage.vertices = new float[rMesh.vertexCount * 3];
					topologyChangedMessage.normals = new float[rMesh.vertexCount * 3];
					topologyChangedMessage.uvs = new float[rMesh.vertexCount * 2];
					memcpy(topologyChangedMessage.vertices, vertices, sizeof(float) * (rMesh.vertexCount * 3));
					memcpy((char*)topologyChangedMessage.normals, normals, sizeof(float) * (rMesh.vertexCount * 3));
					memcpy((char*)topologyChangedMessage.uvs, uvs, sizeof(float) * (rMesh.vertexCount * 2));

					queuedTopologyChangedMessages.push_back(topologyChangedMessage);
				}
			}
			
			//Delete everything that has been allocated
			delete[] vertices;
			delete[] normals;
			delete[] uvs;
		}
	}
}
//---------------------------------------------------------

//Callback function that handles a mesh being removed from the scene
//---------------------------------------------------------
void nodeRemoved(MObject &node, void *clientData)
{
	if (node.hasFn(MFn::kMesh))
	{
		MStreamUtils::stdOutStream() << "Mesh removed\n";
		MFnMesh mMesh(node);

		MStreamUtils::stdOutStream() << mMesh.name() << "\n";

		queuedNodesRemoved.push_back(mMesh.name());
	}	
}
//---------------------------------------------------------

//Callback function that handles a mesh being added in the scene
//---------------------------------------------------------
void nodeAdded(MObject &node, void * clientData)
{
	if (node.hasFn(MFn::kMesh))
	{
		newMeshes.push_back(node);

		MStreamUtils::stdOutStream() << "Node Added from node added callback\n";
		MFnDagNode tempDagNode(node);

		MFnTransform tempTransform(node);

		MString tempTransformName;
		tempTransformName = tempDagNode.name();

		MStreamUtils::stdOutStream() << tempTransformName << "\n";
		MFnMesh mesh(node);

		MDagPath path;
		MFnDagNode(node).getPath(path);

		MCallbackId tempId = MDagMessage::addWorldMatrixModifiedCallback(path, nodeWorldMatrixChanged, NULL, &status);
		callbackIdArray.append(tempId);
	}
}
//---------------------------------------------------------

void timerCallback(float elapsedTime, float lastTime, void *clientData)
{
	//World Matrix Changed
	//------------------------------------------------------------------
	int nrOfWorldMatrixMessages = queuedWorldMatrixMessages.size();			//Get nr of messages
	if (nrOfWorldMatrixMessages > 0)
	{
		size_t msgSize = sizeof(messageHeader) + ((sizeof(modelHeader) + sizeof(Matrix)) * nrOfWorldMatrixMessages);
		messageHeader msgHeader = { "worldMatrixChanged", nrOfWorldMatrixMessages };

		//Memcpy message header to the message
		const char* message = new char[msgSize];
		memcpy((char*)message, &msgHeader, sizeof(messageHeader));

		//Variables to keep track of where to put next message
		int currentMatrix = 0;
		size_t modelMatrixSize = sizeof(modelHeader) + sizeof(Matrix);
		//Loop through messages
		for (int i = queuedWorldMatrixMessages.size() - 1; i >= 0; i--)
		{
			modelHeader mHeader;
			strcpy(mHeader.mModelName, queuedWorldMatrixMessages[i].nodeName.asChar());

			memcpy((char*)message + sizeof(messageHeader) + (modelMatrixSize * currentMatrix), &mHeader, sizeof(modelHeader));
			memcpy((char*)message + sizeof(messageHeader) + (modelMatrixSize * currentMatrix) + sizeof(modelHeader), &queuedWorldMatrixMessages[i].matrixToBeQueued, sizeof(Matrix));

			currentMatrix++;
			queuedWorldMatrixMessages.erase(queuedWorldMatrixMessages.begin() + i);
		}

		if (comLib.send(message, msgSize))
		{
			MStreamUtils::stdOutStream() << "World Transform msg sent" << "\n";
		}
		delete[] message;
	}
	//----------------------------------------------------------------

	//Topology changed
	//----------------------------------------------------------------
	int nrOfTopologyChangedMessages = queuedTopologyChangedMessages.size();
	if (nrOfTopologyChangedMessages > 0)
	{
		for (int i = queuedTopologyChangedMessages.size() - 1; i >= 0; i--)
		{
			int cVertexCount = queuedTopologyChangedMessages[i].vertexCount;
			size_t msgSize = (sizeof(messageHeader) + sizeof(modelHeader) + sizeof(rayLibMeshHeader) + ((sizeof(float) * (cVertexCount * 3)) * 2) + (sizeof(float) * (cVertexCount * 2)));
			messageHeader msgHeader = { "verticesChanged", 1 };
			modelHeader mHeader = {};
			strcpy(mHeader.mModelName, queuedTopologyChangedMessages[i].nodeName.asChar());
			rayLibMeshHeader rMesh = { cVertexCount };

			const char* message = new char[msgSize];

			memcpy((char*)message, &msgHeader, sizeof(messageHeader));
			memcpy((char*)message + sizeof(messageHeader), &mHeader, sizeof(modelHeader));
			memcpy((char*)message + sizeof(messageHeader) + sizeof(modelHeader), &rMesh, sizeof(rayLibMeshHeader));
			memcpy((char*)message + sizeof(messageHeader) + sizeof(modelHeader) + sizeof(rayLibMeshHeader), queuedTopologyChangedMessages[i].vertices, sizeof(float) * (rMesh.vertexCount * 3));
			memcpy((char*)message + sizeof(messageHeader) + sizeof(modelHeader) + sizeof(rayLibMeshHeader) + (sizeof(float) * (rMesh.vertexCount * 3)),
				queuedTopologyChangedMessages[i].normals, sizeof(float) * (rMesh.vertexCount * 3));
			memcpy((char*)message + sizeof(messageHeader) + sizeof(modelHeader) + sizeof(rayLibMeshHeader) + ((sizeof(float) * (rMesh.vertexCount * 3)) * 2),
				queuedTopologyChangedMessages[i].uvs, sizeof(float) * (rMesh.vertexCount * 2));

			if (comLib.send(message, msgSize))
			{
				MStreamUtils::stdOutStream() << "Vertices Changed msg sent" << "\n";
			}

			delete[] message;
			delete[] queuedTopologyChangedMessages[i].vertices;
			delete[] queuedTopologyChangedMessages[i].normals;
			delete[] queuedTopologyChangedMessages[i].uvs;
			queuedTopologyChangedMessages.erase(queuedTopologyChangedMessages.begin() + i);

		}
	}
	//----------------------------------------------------------------

	//Nodes removed
	//----------------------------------------------------------------
	int nrOfNodesRemoved = queuedNodesRemoved.size();					//Get nr of messages
	if (nrOfNodesRemoved > 0)
	{
		size_t msgSize = sizeof(messageHeader) + (sizeof(modelHeader) * nrOfNodesRemoved);		//Get size of full message (messageHeader + all the nodes removed)
		messageHeader msgHeader = { "nodeRemoved", nrOfNodesRemoved };							//Message header with command and nr of nodes that are affected

		const char* message = new char[msgSize];						//Allocate message
		memcpy((char*)message, &msgHeader, sizeof(messageHeader));		//Copy the message header to the message

		int currentNode = 0;											//Variable used to know the offset in the memcpy, based on how many mesh names have been copied
		for (int i = queuedNodesRemoved.size() - 1; i >= 0; i--)		//For loop to get all the nodes that are gonna be removed
		{
			modelHeader mHeader;
			strcpy(mHeader.mModelName, queuedNodesRemoved[i].asChar());		//Get the current mesh name

			memcpy((char*)message + sizeof(messageHeader) + (sizeof(modelHeader) * currentNode), &mHeader, sizeof(modelHeader));		//Memcpy the mesh name

			currentNode++;
			queuedNodesRemoved.erase(queuedNodesRemoved.begin() + i);		//Remove the mesh names from the end of the 
		}

		//Send the message to the rayLib
		if (comLib.send(message, msgSize))
		{
			MStreamUtils::stdOutStream() << "Nodes removed msg sent" << "\n";
		}
		delete[] message;
	}

	//Camera changed--------------------------------------------
	//----------------------------------------------------------
	if (camBool)
	{
		size_t msgSize = sizeof(messageHeader) + (sizeof(Vector3) * 3) + sizeof(int);
		messageHeader msgHeader = { "cameraChanged", 1 };

		const char* message = new char[msgSize];
		memcpy((char*)message, &msgHeader, sizeof(messageHeader));
		memcpy((char*)message + sizeof(messageHeader), &gCamUpDir, sizeof(Vector3));
		memcpy((char*)message + sizeof(messageHeader) + sizeof(Vector3), &gCamTarget, sizeof(Vector3));
		memcpy((char*)message + sizeof(messageHeader) + (sizeof(Vector3) * 2), &gCamPos, sizeof(Vector3));
		memcpy((char*)message + sizeof(messageHeader) + (sizeof(Vector3) * 3), &gIsOrtho, sizeof(int));

		//Send the message to the rayLib
		if (comLib.send(message, msgSize))
		{
			MStreamUtils::stdOutStream() << "Camera changed msg sent" << "\n";
		}
		delete[] message;

		camBool = false;
	}
}

void testFunction(void *clientData)
{
	MStreamUtils::stdOutStream() << "Something Changed =========================" << endl;
}

/*
 * Plugin entry point
 * For remote control of maya
 * open command port: commandPort -nr -name ":1234"
 * close command port: commandPort -cl -name ":1234"
 * send command: see loadPlugin.py and unloadPlugin.py
 */
EXPORT MStatus initializePlugin(MObject obj) {
	MStatus res = MS::kSuccess;
	MFnPlugin myPlugin(obj, "level editor", "1.0", "Any", &res);
	if (MFAIL(res)) {
		CHECK_MSTATUS(res);
		return res;
	}

	// redirect cout to cerr, so that when we do cout goes to cerr
	// in the maya output window (not the scripting output!)
	cout.rdbuf(cerr.rdbuf());

	MStreamUtils::stdOutStream() << "Plugin loaded ===========================\n";

	//register callbacks here
	MCallbackId tempId = MDGMessage::addNodeAddedCallback(nodeAdded, "dependNode", NULL, &status);			//Node added callback
	callbackIdArray.append(tempId);

	tempId = MDGMessage::addNodeRemovedCallback(nodeRemoved, "dependNode", NULL, &status);
	callbackIdArray.append(tempId);

	tempId = MTimerMessage::addTimerCallback(0.05, timerCallback, NULL, &status);							//Timer callback
	callbackIdArray.append(tempId);

	tempId = MDGMessage::addConnectionCallback(vtxPlugConnected, NULL, &status);
	callbackIdArray.append(tempId);

	MItDependencyNodes dgIter(MFn::kCamera);
	MObjectArray cameraObjs;
	while (!dgIter.isDone())
	{
		cameraObjs.append(dgIter.thisNode());
		dgIter.next();
	}

	for (int i = 0; i < cameraObjs.length(); i++)
	{
		MDagPath path;
		MFnDagNode(cameraObjs[i]).getPath(path);
		MFnCamera cam(path);
		MFnTransform camTransform(path);

		MCallbackId tempID;

		tempID = MDagMessage::addWorldMatrixModifiedCallback(path, cameraWorldMatrixChanged, NULL, &status);
		callbackIdArray.append(tempID);
	}
	
	MStringArray eventNames;
	MEventMessage::getEventNames(eventNames);
	MStreamUtils::stdOutStream() << eventNames << endl;

	// ModelPanelSetFocus, modelEditorChanged

	tempId = MEventMessage::addEventCallback("modelEditorChanged", testFunction, NULL, &status);
	callbackIdArray.append(tempId);

	tempId = MUiMessage::addCameraChangedCallback("modelPanel4", camChanged, NULL, &status);
	callbackIdArray.append(tempId);

	// a handy timer, courtesy of Maya
	gTimer.clear();
	gTimer.beginTimer();

	return res;
}

EXPORT MStatus uninitializePlugin(MObject obj) {
	MFnPlugin plugin(obj);
	MStreamUtils::stdOutStream() << "Plugin unloaded =========================" << endl;
	MMessage::removeCallbacks(callbackIdArray);
	return MS::kSuccess;
}