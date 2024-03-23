#ifndef STATICCHECK_H
#define STATICCHECK_H

//�����ڶ����жϵ�ģ����
template<bool>
class CompileSuccess
{
};
//�����ڶ����жϳɹ���ƫ�ػ�
template<>
class CompileSuccess<true>
{
public:
    CompileSuccess(...) {}; //���Խ����κβ����Ĺ��캯��
};

//���������ת�Ĺ��ߺ�����������ֻ��������û�к���ʵ�壬����ռ���ڴ�
int CompileChecker(CompileSuccess<true> const&);

//�����ڶ��Ժ꣬������ֻ������ʱ����������ռ���ڴ�ռ�
//ע�⣺
//	1��expr��boolֵ��
//	2��msg�����ַ���("MyError")��ʽ�ģ����Ǳ�����(MyError)��ʽ�ģ�������ѭ������������׼��
//	3����Assert���񣬵���һ��������������ã�
#define STATIC_CHECK(expr, msg)\
{\
	class CompileError_##msg {};\
	sizeof(CompileChecker(CompileSuccess<false!=(expr)>(CompileError_##msg())));\
}
//�����ǵ�Ԫ���Ժ��� �� ʹ�ø�ʽ����
//void UnitTest_StaticCheck()
//{
//	STATIC_CHECK(false, CheckFailed_CheckItNow);
//	STATIC_CHECK(true, CheckSuccessfully_Pass);
//}
#endif //STATICCHECK_H
