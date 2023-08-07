#include "MsfReader.h"
#include <algorithm>

namespace symbolic_access
{
	MsfReader::MsfReader(FileStream FileStream) : m_FileStream(std::move(FileStream)), m_PageSize(0)
	{
	}

	//��ʼ�� ����pdb�ļ�
	bool MsfReader::Initialize()
	{
		//MsfHeader=Multi-Stream Format
		//����pdb�ļ�ר����ͷ ����DOSͷ
		//�Ȼ�ȡDOSͷ ���ж�ħ��
		MsfHeader msfHeader{};
		if (!m_FileStream.Read(&msfHeader, sizeof(msfHeader)) || msfHeader.Signature != m_Signature)
			return false;

		//PDB�ļ��������� ��ͬ��������ͬ������ 
		//��һЩ�������˷�����Ϣ��һЩ��������������Ϣ
		//һ��ҳ�Ĵ�С
		//��Ŀ¼��һ����������������λ�úʹ�С�� ��С����BLOCK(ҳ)
		//���ǰ�����������

		m_PageSize = msfHeader.PageSize;

		//��Ŀ¼ҳ
		const auto numberOfRootPages = GetNumberOfPages(msfHeader.DirectorySize);
		if (!numberOfRootPages)
			return false;

		//���������Ҫ����block ���д�����ͼȷ����Ҫ��������ҳ���������еĸ�Ŀ¼ҳ��
		//MSF �ļ���Ŀ¼�Ƿ�ҳ�ģ�������Щҳͨ������ҳ������

		const auto numberOfRootIndexPages = GetNumberOfPages(numberOfRootPages * sizeof(uint32_t));
		if (!numberOfRootIndexPages)
			return false;

		kstd::vector<uint32_t> rootPages(numberOfRootPages);
		kstd::vector<uint32_t> rootIndexPages(numberOfRootIndexPages);

		//���ʱ���ļ����Ѿ�Խ��Msf Header��
		//Ҳ����msf header��������������Ŀ¼ҳ����������Ϣ
		//ͬʱ|| ���rootIndexPages�Ƿ���0 ��0�ͷ���
		//�����Ű�rootIndexPages������?
		//pdb�ļ��Ľṹ
		/*
			MSFͷ
			numberOfRootIndexPages��uint32 ָ�����һ��pages������
		*/
		if (!m_FileStream.Read(rootIndexPages.data(), numberOfRootIndexPages * sizeof(uint32_t)) || HasNullOffsets(rootIndexPages))
			return false;

		//��Ŀ¼����ҳ�ж��ٸ�
	
		for (uint32_t i{}, k{}, len{}; i < numberOfRootIndexPages; ++i, k += len)
		{
			//����ҳ����϶��������� һ������4���ֽ�
			//ǰ����һ��ҳ�����ж������� 
			len = min(m_PageSize / 4, numberOfRootPages - k);//Ҳ����һ����ദ��һҳ
			//�ļ���ָ�뵽ָ��λ�� Ҳ�����ܵ�����ҳ  ������� �Ǿ͵�����һ������
			m_FileStream.Seekg(rootIndexPages[i] * m_PageSize);
			if (!m_FileStream.Read(&rootPages[k], len * 4))//��ȡ����ҳ ��ҳ������
				return false;
		}

		if (HasNullOffsets(rootPages))
			return false;

		uint32_t numberOfStreams{};//��ȡ��������
		m_FileStream.Seekg(rootPages[0] * m_PageSize);//��0����ҳ������ָ��Ŀ�(ҳ)��ǰ32λ��������
		if (!m_FileStream.Read(&numberOfStreams, sizeof(numberOfStreams)))
			return false;
		m_Streams.reserve(numberOfStreams);//m_Streams��һ��vector �ṹ��һ�����Ľṹ
		//���ṹ�� 4�ֽڴ�С �����һ���䳤����
		uint32_t currentRootPage{};
		for (uint32_t i{}; i < numberOfRootPages; ++i)
		{
			if(i)//seek����i����ҳ������
				m_FileStream.Seekg(rootPages[i] * m_PageSize);
			//���i==0 k=1,����k=0 Ҳ����˵ֻ�и�ҳ�����ǵ�һ����ʱ�� k����,��Ϊ�������ҳ����ָ���ҳǰ4�ֽ������ĸ���
			for (uint32_t k = i == 0; numberOfStreams > 0 && k < m_PageSize / 4; ++k, --numberOfStreams)
			{//���ֻ��ÿ�δ���һ��ҳ 
				uint32_t size{};
				if (!m_FileStream.Read(&size, sizeof(size)))//�μ�stream�Ľṹ ֻ�и�ҳ������0ָ��ĲŴ�0x4��ʼ
					return false;
				//��ЩRootPages��ʵ������С��ɵ� ������Կ��� RootPages��ǰ������ʵ��������С������
				if (size == 0xFFFFFFFF)
					size = 0;
				//�����������������?
				m_Streams.emplace_back(ContentStream{ size });//��䵽����Ϣ
			}

			if (!numberOfStreams)
			{
				currentRootPage = i;
				break;
			}
		}

		for (auto& stream : m_Streams)
		{
			const auto numberOfPages = GetNumberOfPages(stream.Size);
			if (!numberOfPages)
				continue;

			stream.PageIndices.resize(numberOfPages);//��ȡ��������ŵ����� ����������ռ�

			for (auto remainingPages = numberOfPages; remainingPages > 0;)
			{	//����ҳ��С����ƫ�� Ҳ���Ƕ�����Щ���Ĵ�С ֱ�ӽ����ŴӶ��ĵط�������ʼ
				const auto pageOffset = static_cast<uint32_t>(m_FileStream.Tellg()) % m_PageSize;
				const auto pageSize = min(remainingPages * 4, m_PageSize - pageOffset);//���ܶ���ҳ(����ҳ)
				//��ȡ Ҳ����˵ �����ź��������Щ��������
				if (!m_FileStream.Read(stream.PageIndices.data() + numberOfPages - remainingPages, pageSize))
					return false;
				//��ȥ������
				remainingPages -= pageSize / 4;

				if (pageOffset + pageSize == m_PageSize)//�������� �������������������һ��ҳ
					m_FileStream.Seekg(rootPages[++currentRootPage] * m_PageSize);//Ҫ+1 ��ȡ��һ��Pages ��˷��� 
			}//�Ϳ��Զ���ȫ��������С+��������ŵ�����
		}

		return true;
	}

	kstd::vector<char> MsfReader::GetStream(size_t Index)
	{
		const auto& stream = m_Streams[Index];
		kstd::vector<char> streamData(stream.PageIndices.size() * m_PageSize);

		size_t offset{};
		for (const auto pageIndex : stream.PageIndices)
		{
			m_FileStream.Seekg(pageIndex * m_PageSize);
			m_FileStream.Read(streamData.data() + offset, m_PageSize);

			offset += m_PageSize;
		}

		streamData.resize(stream.Size);
		return streamData;
	}

	//����DirectorySize/һҳ�Ĵ�С Ȼ��+1���߲���
	uint32_t MsfReader::GetNumberOfPages(uint32_t Size)
	{
		const auto numberOfPages = Size / m_PageSize;
		return Size % m_PageSize ? numberOfPages + 1 : numberOfPages;
	}

	//����Ƿ��������������Ƿ���0
	bool MsfReader::HasNullOffsets(const kstd::vector<uint32_t>& Offsets)
	{
		return std::any_of(Offsets.begin(), Offsets.end(), [](uint32_t Offset) { return Offset == 0; });
	}
}