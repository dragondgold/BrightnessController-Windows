// Brightness Controller.cpp : Defines the entry point for the console application.
//
#include "stdafx.h"
#include <atomic>
#include <iostream>
#include <ostream>
#include <boost\asio.hpp>
#include "ScreenBrightController.h"
#include "TCPClient.hpp"

// Comment the following line to disable debug output
#define DEBUG FALSE

#if(DEBUG == TRUE)
	#define debugOUT(s) std::cout << s << std::endl
#else
	#define debugOUT(s)
#endif

#define headerSize	2	// Primer byte indica tipo de dato a recibir y el segundo byte numeros de bytes a recibir

#define configHeader	'I'
#define sampleRate		'S'
#define brightValue		'B'

#pragma comment(lib, "user32.lib")

/*********************************************************************************/
/* Toma una captura de pantalla utilizando GCI y retorna un array de tamaño      */
/*  (width*height) que contiene el color RGB de cada pixel						 */
/*																				 */
/* BmpName		-> Nombre con el que guardar el BitMap en disco					 */
/* height		-> Almacena la altura en pixeles de la captura de pantalla		 */
/* width		-> Almacena el ancho en pixeles de la captura de pantalla		 */
/* bmpToFree    -> Array que se debe liberar despues de hacer uso del array      */
/*                  que devuelve la funcion usando GlobalFree(bmpToFree);        */
/*********************************************************************************/
RGBTRIPLE* ScreenShot(char *BmpName, DWORD &height, DWORD &width, char* &bmpToFree)
{
	HWND DesktopHwnd = GetDesktopWindow();
	RECT DesktopParams;
	HDC DevC = GetDC(DesktopHwnd);
	GetWindowRect(DesktopHwnd, &DesktopParams);
	DWORD Width = DesktopParams.right - DesktopParams.left;
	DWORD Height = DesktopParams.bottom - DesktopParams.top;

	DWORD FileSize = sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)+(sizeof(RGBTRIPLE)+1 * (Width*Height * 4));
	char *BmpFileData = (char*)GlobalAlloc(0x0040, FileSize);

	PBITMAPFILEHEADER BFileHeader = (PBITMAPFILEHEADER)BmpFileData;
	PBITMAPINFOHEADER  BInfoHeader = (PBITMAPINFOHEADER)&BmpFileData[sizeof(BITMAPFILEHEADER)];

	BFileHeader->bfType = 0x4D42; // BM
	BFileHeader->bfSize = sizeof(BITMAPFILEHEADER);
	BFileHeader->bfOffBits = sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER);

	BInfoHeader->biSize = sizeof(BITMAPINFOHEADER);
	BInfoHeader->biPlanes = 1;
	BInfoHeader->biBitCount = 24;
	BInfoHeader->biCompression = BI_RGB;
	BInfoHeader->biHeight = Height;
	BInfoHeader->biWidth = Width;

	RGBTRIPLE *Image = (RGBTRIPLE*)&BmpFileData[sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)];

	HDC CaptureDC = CreateCompatibleDC(DevC);
	HBITMAP CaptureBitmap = CreateCompatibleBitmap(DevC, Width, Height);
	SelectObject(CaptureDC, CaptureBitmap);
	BitBlt(CaptureDC, 0, 0, Width, Height, DevC, 0, 0, SRCCOPY | CAPTUREBLT);
	GetDIBits(CaptureDC, CaptureBitmap, 0, Height, Image, (LPBITMAPINFO)BInfoHeader, DIB_RGB_COLORS);

	height = Height;
	width = Width;
	bmpToFree = BmpFileData;

	//GlobalFree(BmpFileData);		 
	DeleteObject(CaptureBitmap);
	DeleteObject(CaptureDC);

	/*
	DWORD Junk;
	HANDLE FH = CreateFileA(BmpName, GENERIC_WRITE, FILE_SHARE_WRITE, 0, CREATE_ALWAYS, 0, 0);
	WriteFile(FH, BmpFileData, FileSize, &Junk, 0);
	CloseHandle(FH);*/

	return Image;
}

/*********************************************************************************/
/* Calcula el brillo promedio de la imagen mImage pasada						 */
/* mImage	  -> array con los pixeles RGB de la imagen de tamaño (width*height) */
/* height	  -> Almacena la altura en pixeles de la captura de pantalla		 */
/* width	  -> Almacena el ancho en pixeles de la captura de pantalla		     */
/* bmpToFree  -> Array que se debe liberar despues de hacer uso del array        */
/*                  que devuelve la funcion usando GlobalFree(bmpToFree);        */
/*********************************************************************************/
DWORD averageBrightness(RGBTRIPLE *mImage, DWORD height, DWORD width, char* bmpToFree){
	DWORD red = 0, green = 0, blue = 0;
	DWORD samplesNumber = 0;
	DWORD size = height*width;

	LARGE_INTEGER start, end, frequency;
	// get ticks per second
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&start);

	// Muestreo de a 4 pixeles
	for (DWORD n = 0; n < size; n += 4){
		red += mImage[n].rgbtRed;
		green += mImage[n].rgbtGreen;
		blue += mImage[n].rgbtBlue;
		++samplesNumber;
	}

	red = red / samplesNumber;
	green = green / samplesNumber;
	blue = blue / samplesNumber;

	//std::cout << "Red: " << red << std::endl;
	//std::cout << "Green: " << green << std::endl;
	//std::cout << "Blue: " << blue << std::endl;

	// Brillo del color promedio, un número de 0 (oscuro) a 255 (blanco) que indica el brillo
	DWORD brightness = (int)sqrtf(0.241*(float)red*(float)red + 0.691*(float)green*(float)green + 0.068*(float)blue*(float)blue);

	QueryPerformanceCounter(&end);
	//std::cout << "Brightness calculation took: " << ((end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart) << " mS" << std::endl;
	//std::cout << "Brightness: " << brightness << std::endl;

	return brightness;
}

using boost::asio::ip::tcp;
using namespace std::placeholders;

bool initReady = false;							// Inicializacion completa
bool readHeader = true;							// Lectura del header (previo al envio de datos)
std::atomic<bool> newBrightnessValue = false;	// Nuevo valor de brillo para configurar
bool firstSample = true;						

LARGE_INTEGER frequency;						// Ticks por segundo
LARGE_INTEGER t1, t2;							// Ticks

TCPClient *client;								// Cliente TCP para IPC
ScreenBrightController *mController;			// Controlador de brillo
int bValue = 0;									// Valor del brillo
unsigned int sampleDelay;						// Delay entre cada muestreo

/* Callback llamado cuando se reciben datos.
    - bytes_transfered: número de bytes recibidos
	- receiveBuffer: buffer donde se almacenan los bytes recibidos
*/
void readCallback(std::size_t bytes_transfered, std::vector<char> &receiveBuffer){
	if (readHeader){
		if (receiveBuffer[0] == configHeader){
			client->setTransferAtLeast(receiveBuffer[1]);		// Número de bytes a recibir
			readHeader = false;
			debugOUT("Header de configuracion");
		}
	}
	else{
		// Recibo el sample rate (retardo entre cada muestreo en ms). Primero el MSB y luego el LSB
		if (receiveBuffer[0] == sampleRate){
			int16_t data = ((unsigned char)receiveBuffer[1] << 8) | (unsigned char)receiveBuffer[2];
			sampleDelay = data;
			debugOUT("Delay: " << data << std::endl);
			initReady = true;
		}

		// Recibo el brillo que se debe configurar a la pantalla (0 a 100%)
		else if (receiveBuffer[0] == brightValue){
			if (firstSample){
				QueryPerformanceCounter(&t1);
				firstSample = false;
			}

			bValue = receiveBuffer[1];
			newBrightnessValue = true;
			debugOUT("Bright: " << receiveBuffer[1] << std::endl);
		}
		client->setTransferAtLeast(headerSize);
		readHeader = true;
	}
}

int _tmain(int argc, _TCHAR* argv[]) {

	boost::asio::io_service ioService;			// Asio Service
	const std::string server = "localhost";		// Localizacion del servidor (Nombre o IP)
	const std::string port = argv[1];			// Puerto (alias o numero)

	QueryPerformanceFrequency(&frequency);
	debugOUT("Connecting to " << server << " in port " << port);

	client = new TCPClient(ioService, server, port);	// Establezco conexion con el servidor
	client->addReadCallback(readCallback, headerSize);	// Callback para la recepcion de datos

	// Espero a que se conecte al servidor
	while (!client->isConnected());

	debugOUT("Connected to server");

	// Espero por la recepcion de la configuracion
	while (!initReady);
	debugOUT("Init ready" << std::endl);

	// Controlador de brillo
	mController = new ScreenBrightController();		// Controlador de brillo
	DWORD h = 0, w = 0;								// Alto y ancho de la captura (tamaño de pantalla)
	DWORD calculatedBrightnessValue = 0;			// Brillo calculado de la captura
	char* bmpToFree = NULL;							// Bitmap de captura de pantalla

	while (true){

		if (newBrightnessValue){
			if (!firstSample){
				QueryPerformanceCounter(&t2);
				double elapsedTime = (t2.QuadPart - t1.QuadPart) * 1000.0 / frequency.QuadPart;
				std::cout << "Elapsed time: " << elapsedTime << std::endl;
				firstSample = true;
			}
			newBrightnessValue = false;
			mController->setBrightness(bValue);
			debugOUT("Brightness configured" << std::endl);
		}

		RGBTRIPLE *image = ScreenShot("Prueba.bmp", h, w, bmpToFree);				// Captura de pantalla
		calculatedBrightnessValue = averageBrightness(image, h, w, bmpToFree);		// Calculo el brillo promedio de la imagen
		GlobalFree(bmpToFree);														// Libero la memoria del bitmap

		debugOUT("Image Brightness: " << calculatedBrightnessValue);
		
		// Envío el brillo de la imagen para que en base al algoritmo y configuracion reciba el brillo que debo
		//  aplicar a la pantalla
		std::vector<uint8_t> data;
		data.push_back(calculatedBrightnessValue);

		client->Write<uint8_t>(data);
		Sleep(sampleDelay);
	}

	delete client;
	delete mController;
}

