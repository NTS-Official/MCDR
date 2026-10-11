// 下列 ifdef 块是创建使从 DLL 导出更简单的
// 宏的标准方法。此 DLL 中的所有文件都是用命令行上定义的 MCDR_EXPORTS
// 符号编译的。在使用此 DLL 的
// 任何项目上不应定义此符号。这样，源文件中包含此文件的任何其他项目都会将
// MCDR_API 函数视为是从 DLL 导入的，而此 DLL 则将用此宏定义的
// 符号视为是被导出的。
#ifdef MCDR_EXPORTS
#define MCDR_API __declspec(dllexport)
#else
#define MCDR_API __declspec(dllimport)
#endif

// 此类是从 dll 导出的
class MCDR_API CMCDR {
public:
	CMCDR(void);
	// TODO: 在此处添加方法。
};

extern MCDR_API int nMCDR;

MCDR_API int fnMCDR(void);
